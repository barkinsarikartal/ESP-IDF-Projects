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
#include "driver/gpio.h"
#include "esp_private/wifi.h"

#define WIFI_CHANNEL    1
#define TEST_DURATION_S 30
#define PACKET_SIZE     1024
#define ACK_REQUEST     0x01
#define ACK_RESPONSE    0x02
#define STOP_REQUEST    0x03
#define CONT_REQUEST    0x04

#define BOOT_BUTTON_GPIO GPIO_NUM_0

static const char *TAG = "SENDER";
static const char *ESPNOW_TAG = "ESP_NOW";

static bool stop_sending = false;
static bool returned_ack = false;
static bool send_done = true;

int64_t start_time = 0;
int64_t now = 0;
size_t total_sent_bytes = 0;
int packet_count = 0;
uint8_t payload[PACKET_SIZE];
int64_t active_start_us = 0;
int64_t active_duration_us = 0;
bool currently_sending = false;

// static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF8, 0xDE, 0xCC}; //siyah kablolu esp32'nin mac adresi
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF7, 0xB8, 0xF8}; //beyaz kablolu esp32'nin mac adresi
// static uint8_t broadcast_mac[] = {0xF0, 0x9E, 0x9E, 0x20, 0x9A, 0x68}; //ESP32-S3'ün mac adresi

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    send_done = true;
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == 1 && data[0] == ACK_RESPONSE) {
        ESP_LOGW(ESPNOW_TAG, "ACK alındı!");
        returned_ack = true;
    }
}

static void esp_now_send_stop() {
    uint8_t data = STOP_REQUEST;  // duraklama isteği

    esp_err_t res = esp_now_send(broadcast_mac, &data, 1);
    if (res == ESP_OK) {
        ESP_LOGI(ESPNOW_TAG, "STOP isteği gönderildi.\n---");
    }
    else {
        ESP_LOGE(ESPNOW_TAG, "Gönderim hatası: %d", res);
    }
}

static void esp_now_send_cont() {
    uint8_t data = CONT_REQUEST;  // devam etme isteği

    esp_err_t res = esp_now_send(broadcast_mac, &data, 1);
    if (res == ESP_OK) {
        printf("---\n");
        ESP_LOGI(ESPNOW_TAG, "CONT isteği gönderildi.");
    }
    else {
        ESP_LOGE(ESPNOW_TAG, "Gönderim hatası: %d", res);
    }
}

static void esp_now_send_task() {
    ESP_LOGW(ESPNOW_TAG, "ESP-NOW veri gönderme taskı başladı.");
    ESP_LOGW(ESPNOW_TAG, "Throughput testi başlıyor. Duraklatmak / devam ettirmek için BOOT tuşuna basınız.");

    start_time = esp_timer_get_time();
    now = start_time;
    int last_state = 1;

    while (1) {
        int current_state = gpio_get_level(BOOT_BUTTON_GPIO);
        if (last_state == 1 && current_state == 0) {
            stop_sending = !stop_sending;
            if (stop_sending) {
                esp_now_send_stop();
                if (currently_sending) {
                    active_duration_us += esp_timer_get_time() - active_start_us;
                    currently_sending = false;
                }

                double duration_s = active_duration_us / 1000000.0;
                double kb_sent = total_sent_bytes / 1024.0;
                double throughput = kb_sent / duration_s;

                ESP_LOGW(TAG, "TEST DURAKLATILDI");
                ESP_LOGI(TAG, "Toplam gönderilen: %d byte (%d paket)", total_sent_bytes, packet_count);
                ESP_LOGI(TAG, "Aktif gönderim süresi: %.2f saniye", duration_s);
                ESP_LOGI(TAG, "Throughput: %.2f KB/s", throughput);
            }
            else {
                esp_now_send_cont();
            }
        }
        last_state = current_state;

        if (!stop_sending && !currently_sending) {
            // Gönderime yeni başlandı
            active_start_us = esp_timer_get_time();
            currently_sending = true;
        }

        if (stop_sending && currently_sending) {
            // Gönderim durdu
            active_duration_us += esp_timer_get_time() - active_start_us;
            currently_sending = false;
        }

        if (send_done && !stop_sending) {
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
        // esp_task_wdt_reset();
        // vTaskDelay(pdMS_TO_TICKS(2)); //hiç bekleme koymayınca task wdt tetikleniyor
        // esp_rom_delay_us(200); //bekleme süresini 1 milisaniyeden mikrosaniyelere düşürmek throughput'u etkilemedi
    }

    vTaskDelete(NULL);
}

static void esp_now_send_ack() {
    uint8_t data = ACK_REQUEST;

    esp_err_t res = esp_now_send(broadcast_mac, &data, 1);
    if (res != ESP_OK) {
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

    esp_now_rate_config_t rate_cfg = {0};
    rate_cfg.phymode = WIFI_PHY_MODE_HT20;
    rate_cfg.rate = WIFI_PHY_RATE_MCS4_SGI; /** MCS7'de paket kayıpları yaşandı. DeepSeek:
                                             *  Yüksek Modülasyon Karmaşıklığı: MCS7 (64-QAM) yüksek SNR (Signal-to-Noise Ratio)
                                             *  gerektirir. Zayıf sinyalde hata oranı artar.
                                             *  Kısa Koruma Aralığı (SGI): 400ns'lik SGI, çok yollu yansıma (multipath) olan ortamlarda
                                             *  semboller arası girişime (ISI) neden olur.
                                             */
    rate_cfg.ersu = false;

    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(broadcast_mac, &rate_cfg));

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW başlatıldı.");
    
    while (!returned_ack) {  //ack dönene kadar sorgu at
        esp_now_send_ack();
        vTaskDelay(pdMS_TO_TICKS(50));
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

    memset(payload, 0xAA, PACKET_SIZE); // dummy data

    /* GPIO ayarı: BOOT tuşu giriş ve pull-up aktif */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

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