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

#define WIFI_CHANNEL 1
#define TEST_DURATION_S 10
#define PACKET_SIZE 1024

static const char *TAG = "SENDER";
static const char *ESPNOW_TAG = "ESP_NOW";

static bool returned_ack = false;
static bool send_done = true;

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF8, 0xDE, 0xCC}; //siyah kablolu esp32'nin mac adresi
//uint8_t broadcast_mac[] = {0xF0, 0x9E, 0x9E, 0x20, 0x9A, 0x68}; //ESP32-S3'ün mac adresi

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    send_done = true;
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == 1 && data[0] == 0x02) {
        ESP_LOGW(ESPNOW_TAG, "ACK alındı!");
        returned_ack = true;
    }
}

static void esp_now_send_task() {
    ESP_LOGW(ESPNOW_TAG, "ESP-NOW veri gönderme taskı başladı.");

    uint8_t payload[PACKET_SIZE];
    memset(payload, 0xAA, PACKET_SIZE); // dummy data

    int64_t start_time = esp_timer_get_time();
    int64_t now = start_time;
    size_t total_sent_bytes = 0;
    int packet_count = 0;

    while ((now - start_time) < TEST_DURATION_S * 1000000) {
        if (send_done) {
            send_done = false;
            esp_err_t err = esp_now_send(broadcast_mac, payload, PACKET_SIZE);
            if (err == ESP_OK) {
                total_sent_bytes += PACKET_SIZE;
                packet_count++;
                now = esp_timer_get_time();
            }
            else {
                now = esp_timer_get_time();
                ESP_LOGE(TAG, "ESP-NOW Gönderim hatası: %s", esp_err_to_name(err));
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2)); //hiç bekleme koymayınca task wdt tetikleniyor
        // esp_rom_delay_us(200); //bekleme süresini 1 milisaniyeden mikrosaniyelere düşürmek throughput'u etkilemedi
    }

    double duration_s = (now - start_time) / 1000000.0;
    double kb_sent = total_sent_bytes / 1024.0;
    double throughput = kb_sent / duration_s;

    ESP_LOGI(TAG, "TEST TAMAMLANDI");
    ESP_LOGI(TAG, "Toplam gönderilen: %d byte (%d paket)", total_sent_bytes, packet_count);
    ESP_LOGI(TAG, "Süre: %.2f saniye", duration_s);
    ESP_LOGI(TAG, "Throughput: %.2f KB/s", throughput);

    vTaskDelete(NULL);
}

static void esp_now_send_ack() {
    uint8_t data = 0x01;  // ACK isteği

    esp_err_t res = esp_now_send(broadcast_mac, &data, 1);
    if (res == ESP_OK) {
        ESP_LOGI(ESPNOW_TAG, "ACK isteği gönderildi.");
    }
    else {
        ESP_LOGE(ESPNOW_TAG, "Gönderim hatası: %d", res);
    }
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

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW başlatıldı.");

    while (!returned_ack) { //ack dönene kadar saniyede 2 kere sorgu at
        esp_now_send_ack();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    xTaskCreate(esp_now_send_task, "esp_now_send_task", 4096, NULL, 5, NULL);
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

    wifi_init(); // WiFi başlatma

    /* MAC adresini yazdır */
    char macStr[18];
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    ESP_LOGW("MAC", "Bu cihazın (gönderici) mac adresi: %s", macStr);

    esp_now_init_func(); // ESP-NOW başlatma
}