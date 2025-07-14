# ESP32 WebSocket ile Bluetooth Classic Cihaz Tarama (GAP) Projesi

Bu proje, ESP32'nin aynı anda hem WiFi ağına bağlanmasını hem de Bluetooth Classic GAP taraması yapmasını sağlar. Tarama sırasında bulunan cihaz bilgileri (MAC adresi, cihaz adı ve RSSI) JSON formatında WebSocket üzerinden istemciye anlık olarak gönderilir.

- **Bluetooth Classic GAP Taraması**  
  - Çevredeki BR/EDR cihazları taranır.
  - Her cihaz için MAC adresi, ad ve RSSI değeri alınır.
  - BLE (Bluetooth Low Energy) kullanılmaz, sadece klasik Bluetooth aktiftir.

- **WiFi Bağlantısı ve Web Sunucusu**  
  - ESP32, belirlenen bir SSID ve şifre ile WiFi ağına bağlanır.
  - Bağlantı sonrası bir HTTP sunucusu başlatılır.
  - `index.html` istemciye sunulur ve WebSocket üzerinden veri iletimi sağlanır.

- **WebSocket ile JSON Yayını**  
  - Tarama sırasında bulunan cihazlar, her saniye güncellenerek WebSocket ile istemciye gönderilir.
  - JSON formatı:  
    ```json
    {
      "devices": [
        {
          "mac": "00:11:22:33:44:55",
          "name": "DeviceName",
          "rssi": -45
        },
        ...
      ]
    }
    ```

- **Bluetooth + WiFi Coexist Ayarı**  
  - `esp_coex_preference_set(ESP_COEX_PREFER_WIFI);` ile WiFi öncelikli çalışma sağlanır.