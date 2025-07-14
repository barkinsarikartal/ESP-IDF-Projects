/**
 * @brief BR/EDR GAP taraması ile keşfedilen cihazları WebSocket üzerinde listeleyen ESP-IDF uygulaması.
 *
 * Bu program, Bu proje, ESP32 cihazının hem WiFi ağına bağlanıp hem de Bluetooth Classic GAP taraması yapmasını sağlar.
 * WiFi üzerinden bir HTTP sunucusu başlatılır ve WebSocket bağlantısı kurulur.
 * Bluetooth GAP kullanılarak çevredeki BR/EDR cihazları taranır.
 * 25.6 (20 * 1.28) saniyede bir taranan cihazlar temizlenip yeniden tarama yapılır. Bu sayede taranan cihaz listesi hep güncel kalır.
 * Bulunan cihazların MAC adresi, adı ve RSSI bilgileri WebSocket üzerinden JSON formatında istemciye gönderilir.
 * BLE kullanılmaz; sadece klasik Bluetooth (BR/EDR) aktiftir ve WiFi ile birlikte çalışacak şekilde coexist yapılandırılmıştır.
 */

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_bt_defs.h"    //bt definitions (enums, constants etc)
#include "esp_bt_main.h"
#include "esp_spp_api.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_coexist.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <sys/socket.h>     // socket, sendto, sockaddr
#include <netinet/in.h>     // sockaddr_in, htons, IPPROTO_IP, etc.
#include <arpa/inet.h>      // inet_addr
#include <unistd.h>         // close()
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define MAX_DEVICES     30  //bulunan cihazlar dizisinde tutulacak maksimum cihaz sayısı. (ileri geliştirmelerde dinamik dizi yapısı kullanılabilir)
#define BT_DEVICE_NAME  "ESP32_BT"
#define WIFI_SSID       "AT3700_"
#define WIFI_PASS       "12345678"

/* ESP_LOG tag tanımlamaları */
static const char *WSOCKET_TAG = "WEBSOCKET";
static const char *WSERVER_TAG = "WEBSERVER";
static const char *SCAN_TAG = "BT_SCAN";
static const char *BT_TAG = "BT_INIT";
static const char *WIFI_TAG = "WIFI";

esp_event_handler_instance_t instance_any_id;
esp_event_handler_instance_t instance_got_ip;

typedef struct {
    esp_bd_addr_t bda; //bluetooth device address (struct'ın primary keyi)
    char name [ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    int rssi;
    bool name_received;
    bool rssi_received;
    bool printed_to_log;
} device_info_t;

static device_info_t found_devices[MAX_DEVICES];
static int device_count = 0;

static httpd_handle_t server = NULL;
static int client_ws_fd = -1;

TaskHandle_t ws_device_lister_task_handle = NULL;

/**
 * index.html dosyasını SPIFFS yerine programın bin dosyasına
 * gömerek kullanmak için başlangıç ve bitiş adreslerini tanımlama.
 * (Flash’a gömülür ve runtime’da RAM harcamadan okunabilir)
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static char *bda2str(uint8_t * bda, char *str, size_t size) { //BT device address'i stringe çeviren fonksiyon
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void print_device_info(device_info_t *device) {
    char bda_str[18];
    snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
            device->bda[0], device->bda[1], device->bda[2],
            device->bda[3], device->bda[4], device->bda[5]);
    
    ESP_LOGI(SCAN_TAG, "Cihaz adı: %s, MAC: %s, RSSI: %d dBm.",
            device->name, bda_str, device->rssi);
}

static device_info_t* find_or_add_device(esp_bd_addr_t bda) { //found_devices dizisini kontrol eden fonksiyon
    for (int i = 0; i < device_count; i++) {
        if (memcmp(found_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return &found_devices[i];
        }
    }

    if (device_count < MAX_DEVICES) {
        device_info_t *device = &found_devices[device_count];
        memcpy(device->bda, bda, ESP_BD_ADDR_LEN);
        memset(device->name, 0, sizeof(device->name));
        strcpy(device->name, "Unknown");
        device->rssi = 0;
        device->name_received = false;
        device->rssi_received = false;
        device->printed_to_log = false;
        device_count++;
        return device;
    }

    return NULL;
}

static void clear_device_array() { //her taramada sıfırdan taramak için found_devices dizisini temizleyen fonksiyon
    for (int i = 0; i < device_count; i++) {
        device_info_t *device = &found_devices[device_count];
        memset(device->bda, 0, ESP_BD_ADDR_LEN);
        memset(device->name, 0, sizeof(device->name));
        strcpy(device->name, "Unknown");
        device->rssi = 0;
        device->name_received = false;
        device->rssi_received = false;
        device->printed_to_log = false;
    }
    device_count = 0;
}

void ws_device_lister_task() { //periyodik olarak websocket'e json gönderen task
    ESP_LOGW(WSOCKET_TAG, "WS Device Lister Task Created.");
    while (1) {
        if (server != NULL && client_ws_fd >= 0) {
            cJSON *root = cJSON_CreateObject();
            cJSON *array = cJSON_CreateArray();

            for (int i = 0; i < device_count; i++) {
                char bda_adr[18];
                cJSON *dev = cJSON_CreateObject();
                cJSON_AddStringToObject(dev, "mac", bda2str(found_devices[i].bda, bda_adr, 18));
                cJSON_AddStringToObject(dev, "name", found_devices[i].name);
                cJSON_AddNumberToObject(dev, "rssi", found_devices[i].rssi);
                cJSON_AddItemToArray(array, dev);
            }

            cJSON_AddItemToObject(root, "devices", array);

            char *json_str = cJSON_PrintUnformatted(root);
            ESP_LOGI(WSOCKET_TAG, "json str: %s", json_str);
            cJSON_Delete(root);
            httpd_ws_frame_t ws_pkt = {
                .payload = (uint8_t *)json_str,
                .len = strlen(json_str),
                .type = HTTPD_WS_TYPE_TEXT,
                .final = true,
                .fragmented = false,
            };

            if (server != NULL && client_ws_fd != -1) {
                esp_err_t err = httpd_ws_send_frame_async(server, client_ws_fd, &ws_pkt);
                if (err != ESP_OK) {
                    ESP_LOGE(WSOCKET_TAG, "WS gönderimi başarısız: %s", esp_err_to_name(err));
                    client_ws_fd = -1;
                }
            }
            else {
                ESP_LOGW(WSOCKET_TAG, "WebSocket gönderilemedi. server=%p, client_fd=%d", server, client_ws_fd);
            }
            free(json_str);        
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Her saniyede bir dene
    }        
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: { //cihaz bulundu
            device_info_t *device = find_or_add_device(param->disc_res.bda);

            char bda_str[18];
            bda2str(param->disc_res.bda, bda_str, 18);
            
            if (device != NULL) {
                esp_bt_gap_read_remote_name(param->disc_res.bda); //isim alma isteğinde bulun (asenkron)
                esp_bt_gap_read_rssi_delta(param->disc_res.bda); //rssi alma isteği gönder (asenkron)
            }

            break;
        }

        case ESP_BT_GAP_READ_REMOTE_NAME_EVT: { //cihazın adı geldiyse
            device_info_t *device = find_or_add_device(param->disc_res.bda);

            if (device != NULL) {
                device->name_received = true;
                if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) { //cihazın adı okunduysa
                    strncpy(device->name, (char*)param->read_rmt_name.rmt_name, 
                           sizeof(device->name) - 1);
                    device->name[sizeof(device->name) - 1] = '\0';
                }
            }

            break;
        }

        case ESP_BT_GAP_READ_RSSI_DELTA_EVT: { //rssi geldiyse
            device_info_t *device = find_or_add_device(param->read_rssi_delta.bda);
            if (device != NULL) {
                device->rssi_received = true;
                if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
                    device->rssi = param->read_rssi_delta.rssi_delta;
                }
                else {
                    device->rssi = -999; // RSSI alınamadı göstergesi
                }
            }

            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: { //scan (discovery) state değişti ise
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) { //arama zamanı dolduysa
                ESP_LOGW(SCAN_TAG, "Tarama tamamlandı");

                // for (int i = 0; i < device_count; i++) { //bulunan cihazları terminale yazdır
                //     if (!found_devices[i].printed_to_log) {
                //         if (!found_devices[i].rssi_received) {
                //             found_devices[i].rssi = -999; //rssi yok
                //             found_devices[i].rssi_received = true;
                //             if (!found_devices[i].name_received) {
                //                 found_devices[i].name_received = true;
                //             }
                //         }
                //         print_device_info(&found_devices[i]);
                //         found_devices[i].printed_to_log = true;
                //     }
                // }
                // ESP_LOGI(SCAN_TAG, "Toplam %d cihaz bulundu", device_count);
                // ESP_LOGI(SCAN_TAG, "====================");

                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 20, 0); //1 sn bekleyip tekrar tara
            }
            else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) { //arama başladıysa
                clear_device_array();
                ESP_LOGW(SCAN_TAG, "Tarama başlatıldı");
            }

            break;
        }

        default: //diğer eventleri yazdır
            break;
    }
}

esp_err_t index_handler(httpd_req_t *req) {
    const size_t index_html_len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_len);
}

static esp_err_t websocket_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        client_ws_fd = httpd_req_to_sockfd(req);
        server = req->handle;
        ESP_LOGW(WSOCKET_TAG, "Websocket client bağlandı, fd: %d", client_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = NULL;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(WSOCKET_TAG, "Websocket frame alma hatası: %s", esp_err_to_name(ret));
        return ret;
    }

    ws_pkt.payload = malloc(ws_pkt.len + 1);
    if (ws_pkt.payload == NULL) {
        ESP_LOGE(WSOCKET_TAG, "Malloc hatası.");
        return ESP_ERR_NO_MEM;
    }

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    ws_pkt.payload[ws_pkt.len] = '\0';

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(WSOCKET_TAG, "Gelen mesaj: %s", ws_pkt.payload);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGW(WSOCKET_TAG, "Websocket bağlantısı kapandı");
        client_ws_fd = -1;
    }

    free(ws_pkt.payload);
    return ESP_OK;

    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t ws = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = websocket_handler,
        .is_websocket = true,
    };

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &index_uri);
        ESP_LOGW(WSERVER_TAG, "Sunucu başladı ve handler'lar bağlandı");
    }

    return server;
}

void stop_webserver(void) {
    if (server) {
        httpd_stop(server);
        ESP_LOGW(WSERVER_TAG, "Webserver durduruldu.");
        server = NULL;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGW(WIFI_TAG, "WiFi başlatıldı, bağlanıyor...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(WIFI_TAG, "Yeniden bağlantı deneniyor...");
        stop_webserver();
        if (ws_device_lister_task_handle != NULL) {
            vTaskDelete(ws_device_lister_task_handle);
            ws_device_lister_task_handle = NULL;
        }
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGW(WIFI_TAG, "WiFi bağlı. Tarayıcınızda şu IP'yi açın: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGW(WIFI_TAG, "Webserver başlatılıyor.");
        start_webserver();
        xTaskCreate(ws_device_lister_task, "ws_device_lister_task", 4096, NULL, 5, &ws_device_lister_task_handle);
    }
}

void app_main(void) {
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("BT_HCI", ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_NONE);
    esp_log_level_set("BT_RFCOMM", ESP_LOG_ERROR);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_ERROR);
    
    esp_err_t ret = nvs_flash_init(); //nvs başlat
    if (ret) {
        ESP_LOGE(BT_TAG, "NVS Flash init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_coex_preference_set(ESP_COEX_PREFER_WIFI); //co-existence wifi önceliklendirme (ESP_COEX_PREFER_BALANCE, ESP_COEX_PREFER_WIFI, ESP_COEX_PREFER_BT)

    esp_bt_mem_release(ESP_BT_MODE_BLE); //BLE kullanılmayacağı için memory'sini serbest bırak

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT(); //default l2cap, hci yapılandırmaları
    ret = esp_bt_controller_init(&bt_cfg); //bluetooth classic controller'ı default ayarlarla oluştur
    if (ret) {
        ESP_LOGE(BT_TAG, "BT Controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT); //bluetooth classic'i başlat
    if (ret) {
        ESP_LOGE(BT_TAG, "BT Controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init(); //bluedroid (espressif'in bluetooth protocol stack'i): rfcomm ve gap'in çalışmasını sağlar
    if (ret) {
        ESP_LOGE(BT_TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BT_TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_gap_set_device_name(BT_DEVICE_NAME); //isim belirle
    if (ret) {
        ESP_LOGE(BT_TAG, "Cihaz adı ayarlanamadı: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(BT_TAG, "Bluetooth Classic başlatıldı.");
    ESP_LOGW(BT_TAG, "Cihaz adı: %s", BT_DEVICE_NAME);

    ret = esp_bt_gap_register_callback(bt_app_gap_cb); //gap callbacki bağla
    if (ret) {
        ESP_LOGE(BT_TAG, "GAP callback kaydı başarısız: %s", esp_err_to_name(ret));
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 20, 0); //20 x 1.28 = 25.6 saniye boyunca tarama
}