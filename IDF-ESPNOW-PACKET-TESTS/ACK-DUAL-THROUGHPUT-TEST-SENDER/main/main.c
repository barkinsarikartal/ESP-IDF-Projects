#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#define WIFI_CHANNEL    1
#define ACK_TIMEOUT_MS  200
#define ACK_REQUEST     0x01
#define ACK_RESPONSE    0x02
#define PACKET_SIZE     128

static const char *TAG = "SENDER";

static uint8_t broadcast_mac[] = {0xCC, 0x7B, 0x5C, 0xF8, 0xDE, 0xCC}; // Alıcı ESP32 MAC adresi

uint8_t request_payload[PACKET_SIZE];
uint8_t response_payload[PACKET_SIZE];

int total_ack_sent = 0;
int total_ack_received = 0;
SemaphoreHandle_t ack_sem;

int64_t last_send_time = 0;

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Gerekirse loglanabilir
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == PACKET_SIZE && memcmp(data, response_payload, PACKET_SIZE) == 0) {
        xSemaphoreGive(ack_sem);
    }
}

void esp_now_send_ack_loop(void *pvParameters) {
    while (1) {
        int64_t now = esp_timer_get_time();
        
        total_ack_sent++;
        esp_err_t res = esp_now_send(broadcast_mac, request_payload, PACKET_SIZE);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send hatası: %s", esp_err_to_name(res));
        }

        if (xSemaphoreTake(ack_sem, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) == pdTRUE) {
            total_ack_received++;
            int64_t delta_us = now - last_send_time;
            last_send_time = now;
            ESP_LOGI(TAG, "ACK alındı (%d/%d) gecen sure: %.2f millis", total_ack_received, total_ack_sent, delta_us / 1000.0);
        }
        else {
            ESP_LOGW(TAG, "ACK zaman aşımı (%d/%d)", total_ack_received, total_ack_sent);
        }
        //vTaskDelay(pdMS_TO_TICKS(200)); //ACK istekleri arasında bekleme
    }
}

static void esp_now_init_func(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
    
    /* Broadcast peer ekleme*/
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t)); // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html#_CPPv417esp_now_peer_info
    if (peer == NULL) {
        ESP_LOGE(TAG, "Peer bilgileri için malloc başarısız");
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

    ESP_LOGW(TAG, "ESP-NOW baslatildi. Dinlemede.");
}

void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    memset(request_payload, 0x01, PACKET_SIZE); // ACK request filled array
    memset(response_payload, 0x02, PACKET_SIZE); // ACK response filled array

    wifi_init();
    esp_now_init_func();
    ack_sem = xSemaphoreCreateBinary();

    xTaskCreate(esp_now_send_ack_loop, "esp_now_send_ack_loop", 4096, NULL, 5, NULL);
}