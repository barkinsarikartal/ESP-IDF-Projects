#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_private/wifi.h"

#define WIFI_CHANNEL    1
#define TEST_DURATION_S 30
#define PACKET_SIZE     1024
#define ACK_REQUEST     0x01
#define ACK_RESPONSE    0x02
#define STOP_REQUEST    0x03
#define CONT_REQUEST    0x04

static const char *TAG = "RECEIVER";
static const char *ESPNOW_TAG = "ESP_NOW";

static size_t total_received_bytes = 0;
static int64_t start_time_us = 0;
static bool ack_completed = false;
static bool stop_received = false;
int64_t active_start_us = 0;
int64_t active_duration_us = 0;
bool currently_receiving = false;

static uint8_t broadcast_mac[] = {0xF0, 0x9E, 0x9E, 0x20, 0x9A, 0x68}; //ESP32-S3'ün mac adresi
// static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF8, 0xDE, 0xCC}; //siyah kablolu esp32'nin mac adresi

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(ESPNOW_TAG, "ACK Gonderimi basarili");
    }
    else {
        ESP_LOGW(ESPNOW_TAG, "ACK Gonderimi basarisiz.");
    }
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (!stop_received && !ack_completed && len == 1 && data[0] == ACK_REQUEST) {
        ESP_LOGI(ESPNOW_TAG, "ACK isteği alındı, yanıt gönderiliyor...");
        uint8_t ack = ACK_RESPONSE;
        esp_err_t err = esp_now_send(broadcast_mac, &ack, 1);
        if (err == ESP_OK) {
            ack_completed = true;
            ESP_LOGW(ESPNOW_TAG, "Throughput testi başlıyor. Duraklatmak / devam ettirmek için vericideki BOOT tuşuna basınız.");
            start_time_us = esp_timer_get_time();
        }
        else {
            ESP_LOGE(ESPNOW_TAG, "Veri gönderim hatası: %s", esp_err_to_name(err));
        }
    }

    if (!stop_received && ack_completed && len == PACKET_SIZE) {
        total_received_bytes += len;
    }

    if (ack_completed && len == 1) {
        switch(data[0]) {
            case(STOP_REQUEST):
                ESP_LOGI(ESPNOW_TAG, "STOP isteği alındı, test duraklatılıyor...\n---");
                stop_received = true;
                break;
            case(CONT_REQUEST):
                printf("---\n");
                ESP_LOGI(ESPNOW_TAG, "CONT isteği alındı, test devam ettiriliyor...");
                stop_received = false;
                break;
            default:
                break;
        }
    }
}

static void esp_now_recv_task() {
    while (1) {
        if (ack_completed && !stop_received) {
            // Yeni bir gönderim süreci başladıysa
            if (!currently_receiving) {
                active_start_us = esp_timer_get_time();
                currently_receiving = true;
            }
        }
        else if (ack_completed && currently_receiving && stop_received) {  // verici ESP32'nin paket gönderiği süreyi tam kapsayabilmek için gönderim süresinden 0.05 saniye daha fazla dinleme yapıyor
            // Gönderim durdu → süreyi ekle
            active_duration_us += esp_timer_get_time() - active_start_us;
            currently_receiving = false;

            double duration_s = active_duration_us / 1000000.0;
            double kb_received = total_received_bytes / 1024.0;
            double throughput = kb_received / duration_s;

            ESP_LOGW(TAG, "TEST DURAKLATILDI");
            ESP_LOGI(TAG, "Şimdiye kadar alınan veri: %d byte (%d paket)", total_received_bytes, total_received_bytes / 1024);
            ESP_LOGI(TAG, "Süre: %.2f saniye", duration_s);
            ESP_LOGI(TAG, "Throughput: %.2f KB/s", throughput);
            while(stop_received);
                // vTaskDelay(pdMS_TO_TICKS(1));
        }
        // vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(NULL);
}

static void esp_now_init_func(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
    
    /* Broadcast peer ekleme*/
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t)); // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html#_CPPv417esp_now_peer_info
    if (peer == NULL) {
        ESP_LOGE(ESPNOW_TAG, "Peer bilgileri için malloc başarısız");
        return;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = WIFI_CHANNEL;  /**
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

    esp_now_rate_config_t rate_cfg = {0};
    rate_cfg.phymode = WIFI_PHY_MODE_HT20;
    rate_cfg.rate = WIFI_PHY_RATE_MCS4_SGI;
    rate_cfg.ersu = false;

    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(broadcast_mac, &rate_cfg));

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW baslatildi. Dinlemede.");
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init()); // NVS başlatma

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
    xTaskCreate(esp_now_recv_task, "esp_now_recv_task", 4096, NULL, 5, NULL); // dinleyici task'ını başlat
}