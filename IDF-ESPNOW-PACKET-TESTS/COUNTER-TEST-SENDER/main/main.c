/**
 * Bu demo, ESP-NOW üzerinden 1024 byte'lık chunk'lar halinde
 * veri yağmuru olduğu zaman kayıp olup olmadığını, kayıp oluyorsa
 * kayıp miktarını ölçmek için yazılmıştır. Bu C dosyası demonun
 * gönderici cihaz için kodunu içerir.
*/

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

#define ESP_NOW_DATA_LEN 1024
uint8_t stress_buf[ESP_NOW_DATA_LEN];
bool returned_ack = false;

static const char *ESPNOW_TAG = "ESP_NOW";

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xF0, 0x9E, 0x9E, 0x20, 0x9A, 0x68}; //ESP32-S3'ün mac adresi
//static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF7, 0xB8, 0xF8}; //beyaz kablolu esp32'nin mac adresi
//static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; //broadcast mac

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // char macStr[18];
    // snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
    //          mac_addr[0], mac_addr[1], mac_addr[2],
    //          mac_addr[3], mac_addr[4], mac_addr[5]);
    // ESP_LOGW(ESPNOW_TAG, "MAC: %s, Gönderim durumu: %s", macStr, status == ESP_NOW_SEND_SUCCESS ? "Başarılı" : "Başarısız");
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    // char macStr[18];
    // snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
    //          recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
    //          recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);

    // ESP_LOGI(ESPNOW_TAG, "Gelen veri (%d byte) MAC %s:", len, macStr);
    // ESP_LOG_BUFFER_HEXDUMP(ESPNOW_TAG, data, len, ESP_LOG_INFO);

    if (len == 1 && data[0] == 0x02) {
        ESP_LOGI(ESPNOW_TAG, "ACK alındı!");
        returned_ack = true;
    }
}

static void esp_now_send_ack() {
    uint8_t data = 0x01;  // ACK isteği

    esp_err_t res = esp_now_send(broadcast_mac, &data, 1);
    if (res == ESP_OK) {
        ESP_LOGW(ESPNOW_TAG, "ACK isteği gönderildi.");
    }
    else {
        ESP_LOGE(ESPNOW_TAG, "Gönderim hatası: %d", res);
    }
}

static void esp_now_send_task() {
    ESP_LOGW(ESPNOW_TAG, "ESP-NOW veri gönderme taskı başladı.");
    static int success_counter = 0, fail_counter = 0, try_counter = 0;
    
    while(1) {
        esp_err_t result = esp_now_send(broadcast_mac, (uint8_t *)&stress_buf, sizeof(stress_buf)); //önceden belirlenen MAC adresine gelen veriyi gönder
        try_counter++;
        if (result == ESP_OK) {
            success_counter++;
        }
        else {
            ESP_LOGE(ESPNOW_TAG, "Veri gönderim hatası: %s", esp_err_to_name(result));
            fail_counter++;
            break;
        }
        ESP_LOGW(ESPNOW_TAG, "total try: %d", try_counter);
        vTaskDelay(pdMS_TO_TICKS(10));  //100 ms'ten 10 ms'e düşürdükten birkaç dakika sonra alıcı ESP32'de ciddi sıcaklık artışı gözlendi
    }
    ESP_LOGW(ESPNOW_TAG, "total try: %d, success: %d, fail: %d", try_counter, success_counter, fail_counter);
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

    ESP_LOGW(ESPNOW_TAG, "ESP-NOW başlatıldı.");
    esp_now_send_ack();

    while(!returned_ack); //ack dönene kadar bekle (bu iyi bir yöntem değil. wdt tetiklenmesine neden oluyor ama test için yeterli.)
    xTaskCreate(esp_now_send_task, "esp_now_send_task", 4096, NULL, 5, NULL);
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

    memset(stress_buf, 1, ESP_NOW_DATA_LEN);
    ESP_LOGI("BUF", "sizeof stress_buf = %d", sizeof(stress_buf));
    
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