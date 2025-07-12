#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"

static const char *ESPNOW_TAG = "ESP_NOW";

/* Gönderilecek - alınacak veri yapısı */
typedef struct {
    int id;
    char message[100];
} esp_now_data_t;

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) { // ESP-NOW alım callback fonksiyonu
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
             recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
    ESP_LOGW(ESPNOW_TAG, "Veri alindi, gonderen MAC: %s, uzunluk: %d, gonderen RSSI: %d", macStr, len, recv_info->rx_ctrl->rssi);
    
    if (len == sizeof(esp_now_data_t)) {
        esp_now_data_t *recv_data = (esp_now_data_t *)data;
        ESP_LOGW(ESPNOW_TAG, "ID: %d, Mesaj: %s\n", 
                 recv_data->id, recv_data->message);
    }
}

static void esp_now_init_func(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW baslatildi. Dinlemede.");
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
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
    ESP_LOGW("MAC", "Bu cihazin (dinleyici) mac adresi: %s", macStr);
}