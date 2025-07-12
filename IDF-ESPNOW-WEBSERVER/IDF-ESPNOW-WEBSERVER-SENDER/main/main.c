#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#define WIFI_SSID "ESPNOW-WEBSERVER"
#define WIFI_PASS "51575570"

static const char *WSERVER_TAG = "WEBSERVER";
static const char *ESPNOW_TAG = "ESP_NOW";
static const char *WIFI_TAG = "WIFI";

static httpd_handle_t server = NULL;

esp_event_handler_instance_t instance_any_id;
esp_event_handler_instance_t instance_got_ip;

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xF0, 0x9E, 0x9E, 0x20, 0x9A, 0x68}; //ESP32-S3'ün mac adresi
//static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF7, 0xB8, 0xF8}; //beyaz kablolu esp32'nin mac adresi
//static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; //broadcast mac

/* Gönderilecek - alınacak veri yapısı */
typedef struct {
    int id;
    char message[100];
} esp_now_data_t;

/**
 * index.html dosyasını SPIFFS yerine programın bin dosyasına
 * gömerek kullanmak için başlangıç ve bitiş adreslerini tanımlama.
 * (Flash’a gömülür ve runtime’da RAM harcamadan okunabilir)
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) { // ESP-NOW gönderim callback fonksiyonu
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGW(ESPNOW_TAG, "MAC: %s, Gönderim durumu: %s", macStr, status == ESP_NOW_SEND_SUCCESS ? "Başarılı" : "Başarısız");
}

static void esp_now_send_func(char* msg) {
    esp_now_data_t send_data;
    static int counter = 0;
    
    send_data.id = ++counter;
    snprintf(send_data.message, sizeof(send_data.message), msg);
    
    esp_err_t result = esp_now_send(broadcast_mac, (uint8_t *)&send_data, sizeof(send_data)); //önceden belirlenen MAC adresine gelen veriyi gönder
    if (result == ESP_OK) {
        ESP_LOGW(ESPNOW_TAG, "Veri gönderildi: ID=%d, message: %s", send_data.id, send_data.message);
    }
    else {
        ESP_LOGE(ESPNOW_TAG, "Veri gönderim hatası: %s", esp_err_to_name(result));
    }
}

static void esp_now_init_func(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));
    
    /* Broadcast peer ekleme*/
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t)); // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html#_CPPv417esp_now_peer_info
    if (peer == NULL) {
        ESP_LOGE(ESPNOW_TAG, "Peer bilgileri için malloc başarısız");
        return;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = 1;          /**
                                 * Hangi wifi kanalı ile iletişim kurulacak?
                                 * Wi-Fi channel that peer uses to send/receive ESPNOW data.
                                 * If the value is 0, use the current channel which station
                                 * or softap is on. Otherwise, it must be set as the channel
                                 * that station or softap is on.
                                 */
    peer->ifidx = WIFI_IF_STA;  // Karşıdaki cihaz değil, bu cihazın hangi wifi arayüzü ile veri göndereceği (Claude: Hedef cihaza hangi arayüz üzerinden (STA/AP) ulaşacağınız)
    peer->encrypt = false;      // Cihazlar arası veri şifrelemesi olacak mı?
    memcpy(peer->peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    free(peer);

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW başlatıldı. Veri göndermeye hazır.");
}

esp_err_t index_handler(httpd_req_t *req) {
    const size_t index_html_len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_len);
}

esp_err_t submit_post_handler(httpd_req_t *req) {
    char content[100];
    int ret = httpd_req_recv(req, content, MIN(req->content_len, sizeof(content) - 1)); //verinin uzunluğuna göre ya array size kadar ya da veri uzunluğu kadar oku
    if (ret <= 0) {
        return ESP_FAIL;
    }

    content[ret] = '\0'; // Null terminate
    ESP_LOGI(WSERVER_TAG, "Gelen veri: %s", content);

    esp_now_send_func(content); //gelen veriyi espnow ile gönder

    /* Formun yeniden doldurulabilmesi için HTTP 303 ile index'e otomatik yönlendirme */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, ""); // Boş cevap body
}

static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = { //index.html uri
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t submit_uri = { //form submit uri
        .uri      = "/submit",
        .method   = HTTP_POST,
        .handler  = submit_post_handler,
        .user_ctx = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &submit_uri);
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

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGW(WIFI_TAG, "SSID'ye bir cihaz bağlandı.");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGW(WIFI_TAG, "Bir cihazın bağlantısı kesildi.");
    }
}

void print_ap_ip() { //AP'ye bağlandıktan sonra tarayıcıdan açılacak ip'yi yazdıran fonksiyon
    esp_netif_ip_info_t ip_info;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGW(WIFI_TAG, "SoftAP IP: " IPSTR, IP2STR(&ip_info.ip));
    }
    else {
        ESP_LOGE(WIFI_TAG, "SoftAP IP bilgisi alınamadı.");
    }
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap(); //soft ap modu

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); //espnow + ap için dual mode gerekli
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL)); //wifi event handler'ı bağla
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(WIFI_TAG, "SoftAP başlatıldı. SSID: %s, PASS: %s", ap_config.ap.ssid, ap_config.ap.password);
    if (start_webserver() == NULL) {
        ESP_LOGE(WSERVER_TAG, "Web sunucusu başlatılamadı!");
    }
    print_ap_ip();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init(); // NVS başlatma
    if (ret) {
        ESP_LOGE("MAIN", "nvs init fail.");
        return;
    }
    ESP_ERROR_CHECK(ret);
    
    /* WiFi ve ESP-NOW başlatma */
    wifi_init();
    esp_now_init_func();
    
    /* MAC adresini yazdır */
    char macStr[18];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    ESP_LOGW("MAC", "Bu cihazın (gönderici) mac adresi: %s", macStr);
}