/**
 * Bu demo, ESP-NOW üzerinden 1024 byte'lık chunk'lar halinde
 * veri yağmuru olduğu zaman kayıp olup olmadığını, kayıp oluyorsa
 * kayıp miktarını ölçmek için yazılmıştır. Bu C dosyası demonun
 * alıcı cihaz için kodunu içerir.
*/

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

static int success_counter = 0;
static bool ack_completed = false;

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xCC, 0x7B, 0x5C, 0xF8, 0xDE, 0xCC}; //siyah kablolu esp32'nin mac adresi

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGW(ESPNOW_TAG, "MAC: %s, Gonderim durumu: %s", macStr, status == ESP_NOW_SEND_SUCCESS ? "Basarili" : "Basarisiz");
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (ack_completed && len == 1024){
        success_counter++;
        ESP_LOGI(ESPNOW_TAG, "received chunks: %d", success_counter);
    }
    
    if (!ack_completed && len == 1 && data[0] == 0x01) {
        ESP_LOGI(ESPNOW_TAG, "ACK isteği alındı, yanıt gönderiliyor...");
        success_counter = 0; //ack istenmişse verici cihaz resetlenmiş demektir
        uint8_t ack = 0x02;
        esp_now_send(broadcast_mac, &ack, 1);
        ack_completed = true;
    }
}

static void esp_now_init_func(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
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