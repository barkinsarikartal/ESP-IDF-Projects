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

#define AP_NAME "ESP32_Webserver"
#define AP_PASS "12345678"

esp_err_t html_get_handler(httpd_req_t *req) {
    size_t heap_before = esp_get_free_heap_size();
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        ESP_LOGE("handler", "index.html açılamadı");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    char buf[256];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    uint32_t end = xTaskGetTickCount() * portTICK_PERIOD_MS;
    size_t heap_after = esp_get_free_heap_size();


    printf("HTML Heap used: %d bytes\n", (int)(heap_before - heap_after));
    printf("HTML time elapsed (ms): %ld\n", (end-start));
    
    return ESP_OK;
}

esp_err_t gzip_get_handler(httpd_req_t *req) {
    size_t heap_before = esp_get_free_heap_size();
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    FILE *f = fopen("/spiffs/index.html.gz", "r");
    if (!f) {
        ESP_LOGE("handler", "index.html.gz açılamadı");
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
    httpd_resp_send_chunk(req, NULL, 0);

    uint32_t end = xTaskGetTickCount() * portTICK_PERIOD_MS;
    size_t heap_after = esp_get_free_heap_size();

    printf("GZIP Heap used: %d bytes\n", (int)(heap_before - heap_after));
    printf("GZIP time elapsed (ms): %ld\n", (end-start));
    
    return ESP_OK;
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
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t gzip_uri = {
            .uri      = "/index.html.gz",
            .method   = HTTP_GET,
            .handler  = gzip_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t html_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = html_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &gzip_uri);
        httpd_register_uri_handler(server, &html_uri);
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
}