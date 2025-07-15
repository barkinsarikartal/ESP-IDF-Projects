/**
 * @brief WebSocket ile dosya yükleme ilerleme durumunu anlık olarak gösteren ESP-IDF uygulaması.
 *
 * Bu proje, ESP32 cihazı üzerinde bir HTTP sunucusu çalıştırarak kullanıcı tarafından yüklenen
 * dosyanın anlık ilerlemesini WebSocket üzerinden istemciye bildirir. Dosya yükleme işlemi
 * sırasında heap kullanımı gözlemlenir ve gzip ile sıkıştırılmış bir HTML arayüzü sunulur.
 *
 * Web arayüzü `/` URI'sinden yüklenir, dosya yükleme `/upload`, WebSocket iletişimi ise `/ws` URI'si üzerinden yapılır.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

#define CHUNK_SIZE  4096    //Dosyadan tek seferde okunacak byte sayısı
#define WIFI_SSID   "ESP32_WS_FILE_UPLOAD"
#define WIFI_PASS   "12345678"

#define DEBUG_HEAP_LOGS 1   //Upload süresince heap durumunu gözlemlemek için

/* ESP_LOG tag tanımlamaları */
static const char *UPLOAD_TAG = "UPLOAD_HANDLER";
static const char *INDEX_TAG = "INDEX_HANDLER";
static const char *WIFI_TAG = "WIFI_EVENTS";
static const char *SOCKET_TAG = "WEBSOCKET";
static const char *SERVER_TAG = "WEBSERVER";
static const char *SPIFFS_TAG = "SPIFFS";
static const char *HEAP_TAG = "HEAP";

extern int ws_client_fd = -1;
httpd_handle_t server = NULL;

/* Dosya alımı sırasında heap değişimini gözlemlemek için değişkenler */
unsigned long start_heap = 0;
unsigned long receive_heap = 0;

static void send_progress_via_ws(size_t bytes_received, size_t total_size) { //dosya alım sürecini JSON'a çevirip websocket bağlantısına ileten fonksiyon
    if (ws_client_fd < 0) return;

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"received\": %d, \"total\": %d}", (int)bytes_received, (int)total_size);

    httpd_ws_frame_t frame = {
        .payload = (uint8_t *)msg,
        .len = strlen(msg),
        .type = HTTPD_WS_TYPE_TEXT,
        .final = true
    };
    httpd_ws_send_frame_async(server, ws_client_fd, &frame);
}

esp_err_t index_get_handler(httpd_req_t *req) {
    FILE *f = fopen("/spiffs/index.html.gz", "r");
    if (!f) {
        ESP_LOGE(INDEX_TAG, "index.html.gz açılamadı");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char buf[256];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); //index.html.gz sonu
    
    return ESP_OK;
}

esp_err_t upload_post_handler(httpd_req_t *req) {
    start_heap = esp_get_free_heap_size();

    char *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        ESP_LOGE(UPLOAD_TAG, "Buffer için yeterli bellek yok!");
        return ESP_FAIL;
    }

    int received;
    size_t total_fetched = 0;
    bool header_ended = false;

    ESP_LOGW(UPLOAD_TAG, "Dosya alımı başladı.");

    while ((received = httpd_req_recv(req, buf, CHUNK_SIZE)) > 0) { //veri geldikçe okumaya devam et
        if (received < CHUNK_SIZE) buf[received] = '\0';

        if (!header_ended) {
            char *body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                int body_len = received - (body_start - buf);
                if (body_len > 0) {
                    ESP_LOGI(UPLOAD_TAG, "Gelen parça (%d byte): \n%.*s", body_len, body_len, body_start);
                }
                header_ended = true;
            }
        }
        else {
            ESP_LOGI(UPLOAD_TAG, "Gelen parça (%d byte): \n%.*s", received, received, buf);
        }
        total_fetched += received;
        ESP_LOGI(UPLOAD_TAG, "Websocket progress gönderiliyor: %d, %d", total_fetched, req->content_len);
        send_progress_via_ws(total_fetched, req->content_len); //şimdiye kadar kaç byte okunduğunu websocket ile gönder
        receive_heap = esp_get_free_heap_size();
        #ifdef DEBUG_HEAP_LOGS
            ESP_LOGI(HEAP_TAG, "Başlangıç heap: %lu, güncel heap: %lu, (start_heap - receive_heap): %lu", start_heap, receive_heap, (start_heap - receive_heap));
        #endif
    }

    if (received < 0) {
        ESP_LOGE(UPLOAD_TAG, "Dosya alım hatası: %d", received);
        free(buf);
        return ESP_FAIL;
    }

    if (total_fetched != req->content_len) {
        ESP_LOGE(UPLOAD_TAG, "Dosya alımı tamamlanamadı");
        httpd_resp_sendstr(req, "File upload failed.");
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGW(UPLOAD_TAG, "Dosya alımı tamamlandı");
    httpd_resp_sendstr(req, "File uploaded successfully.");
    free(buf);
    return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGW(SOCKET_TAG, "Websocket handshake tamamlandı");
        ws_client_fd = httpd_req_to_sockfd(req); //istemcinin socket numarasını al
        server = req->handle;
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt; //gelen ws frame'i
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0); //gelen ws çerçevesini al
    if (ret != ESP_OK) {
        ESP_LOGE(SOCKET_TAG, "ws receive size failed: %d", ret);
        return ret;
    }

    //frame size alındıysa
    if (ws_pkt.len) {
        uint8_t *buf = malloc(ws_pkt.len + 1); //mesaj + null char
        if (buf) {
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret == ESP_OK) {
                buf[ws_pkt.len] = 0;
                ESP_LOGW(SOCKET_TAG, "Gelen ws mesajı: %s", buf);
            }
            free(buf);
        }
    }
    return ESP_OK;
}

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGW(WIFI_TAG, "SSID'ye bir cihaz bağlandı.");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGW(WIFI_TAG, "Bir cihazın bağlantısı kesildi.");
    }
}

void mount_spiffs(void) { //SPIFFS'i bağlayan fonksiyon
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGW(SPIFFS_TAG, "SPIFFS bağlandı");
}

void read_spiffs(void) { //SPIFFS bilgilerini terminale yazdıran fonksiyon
    DIR* dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE(SPIFFS_TAG, "/spiffs dizinini açma başarısız!");
        return;
    }

    struct dirent* entry;
    struct stat entry_stat;
    char full_path[265];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(full_path, sizeof(full_path), "/spiffs/%s", entry->d_name);
        
        if (stat(full_path, &entry_stat) == -1) {
            ESP_LOGE(SPIFFS_TAG, "%s - bilgi alınamadı", entry->d_name);
            continue;
        }
        
        if (S_ISDIR(entry_stat.st_mode)) {
            ESP_LOGW(SPIFFS_TAG, "[DİZİN] %s", entry->d_name);
        }
        else {
            ESP_LOGW(SPIFFS_TAG, "%-20s - %7d bytes", entry->d_name, (int)entry_stat.st_size);
        }
    }
    
    closedir(dir);
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = httpd_start(&server, &config);

    if (ret == ESP_OK) {
        httpd_uri_t index_uri = { //Index sayfası URI'si
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_get_handler,
            .user_ctx = NULL
        };

        httpd_uri_t upload_uri = { //Dosya yükleme URI'si
            .uri = "/upload",
            .method = HTTP_POST,
            .handler = upload_post_handler,
        };

        httpd_uri_t ws_uri = { //Websocket URI'si
            .uri       = "/ws",
            .method    = HTTP_GET,
            .handler   = ws_handler,
            .user_ctx  = NULL,
            .is_websocket = true
        };

        /* URI'leri server'a kaydet */
        httpd_register_uri_handler(server, &ws_uri);
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &upload_uri);

        ESP_LOGW(SERVER_TAG, "Webserver başladı ve handler'lar bağlandı");
        return server;
    }
    else {
        ESP_LOGE(SERVER_TAG, "Sunucu başlatma hatası! Hata: %s", esp_err_to_name(ret));
    }
    return NULL;
}

void stop_webserver(httpd_handle_t server) {
    if (server) {
        httpd_stop(server);
        ESP_LOGW(SERVER_TAG, "Webserver durduruldu.");
    }
}

void app_main(void) {
    /* NVS ve SPIFFS başlatma */
    ESP_ERROR_CHECK(nvs_flash_init());
    mount_spiffs();
    read_spiffs();

    /* WiFi ve SoftAP Ayarları */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW("MAIN", "SoftAP başlatıldı. SSID: %s, PASS: %s", ap_config.ap.ssid, ap_config.ap.password);

    start_webserver();
}