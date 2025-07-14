#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
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

#define AP_NAME "ESP32_WEBSOCKET"
#define AP_PASS "12345678"

static const char *TAG = "websocket";

static httpd_handle_t server = NULL;
static httpd_handle_t ws_server_handle = NULL;

static int ws_client_fd = -1;
static int client_connected = 0;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake tamamlandı");
        ws_client_fd = httpd_req_to_sockfd(req);
        client_connected = 1;
        return ESP_OK;
    }
    return ESP_OK;
}

void ws_send_task(void *arg) {
    ESP_LOGI("task", "task created and running.");
    while (1) {
        if (client_connected) {
            char message[32];
            float temp = (rand() % 1000) / 10.0; // 0.00 - 99.99
            snprintf(message, sizeof(message), "%.2f", temp);

            httpd_ws_frame_t ws_pkt = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)message,
                .len = strlen(message),
            };
            httpd_ws_send_data(server, ws_client_fd, &ws_pkt);
            ESP_LOGI("socket", "data sent.");
        }
        else {
            ESP_LOGI("socket", "client is not connected.");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI("wifi_events", "SSID'ye bir cihaz bağlandı.");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI("wifi_events", "bir cihazın SSID'den bağlantısı kesildi.");
    }
    else {
        ESP_LOGI("wifi_events", "diğer wifi olayı: %ld.", id);
    }
}

void mount_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGI("spiffs", "SPIFFS mounted");
}

void read_spiffs(void) {
    DIR* dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE("spiffs","directory açma başarısız!");
        return;
    }

    struct dirent* entry;
    struct stat entry_stat;
    char full_path[265];
    
    while ((entry = readdir(dir)) != NULL) {
        // "." ve ".." girişlerini atla
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Tam dosya yolunu oluştur
        snprintf(full_path, sizeof(full_path), "/spiffs/%s", entry->d_name);
        
        // Dosya bilgilerini al
        if (stat(full_path, &entry_stat) == -1) {
            ESP_LOGW("SPIFFS", "%s - bilgi alınamadı", entry->d_name);
            continue;
        }
        
        // Dizin mi dosya mı kontrol et
        if (S_ISDIR(entry_stat.st_mode)) {
            ESP_LOGI("SPIFFS", "[DİZİN] %s", entry->d_name);
        } else {
            ESP_LOGI("SPIFFS", "%-20s - %7d bytes", entry->d_name, (int)entry_stat.st_size);
        }
    }
    
    closedir(dir);
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &ws_uri);
        ws_server_handle = server;
        ESP_LOGI("web_server", "sunucu başladı ve handler'lar bağlandı");
        return server;
    }
    ESP_LOGI("web_server", "sunucu başlamadı");
    return NULL;
}

void stop_webserver(httpd_handle_t server) {
    if (server) {
        httpd_stop(server);
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    mount_spiffs();
    read_spiffs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_NAME,
            .password = AP_PASS,
            .ssid_len = strlen(AP_NAME),
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("main", "SoftAP sunucu başlatıldı. SSID: WebServer_Deneme");

    start_webserver();
    xTaskCreate(ws_send_task, "ws_send_task", 4096, NULL, 5, NULL);
}