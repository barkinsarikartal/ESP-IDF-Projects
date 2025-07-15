# ESP32 WebSocket Dosya Yükleme

Bu proje, ESP32 üzerinde çalışan bir web sunucusu ile dosya yükleme işlemini gerçekleştirir. Dosya yüklenirken, ilerleme durumu WebSocket üzerinden istemciye anlık olarak iletilirken dosya içeriği de ESP-IDF terminaline yazdırılır.

Yüklenen dosya, SPIFFS benzeri bir dosya sistemine kaydedilmeden seri terminal, UART veya BR/EDR SPP gibi haberleşme kanalları üzerinden stream olarak iletilebilir.

ESP32, kendi WiFi ağı üzerinden istemciye `index.html.gz` dosyasını sunar. Dosya yükleme işlemi tamamlandığında istemci bilgilendirilir.

Cihaz açıldığında şu bilgilerle bir WiFi ağı oluşturur:

- SSID: ESP32_WS_FILE_UPLOAD  
- Şifre: 12345678

# NOTLAR:

- Projeyi derlemeden önce menuconfig üzerinden "Flash Size" seçeneğinin 4MB değerine ve ve "Partition Table" seçeneğinin "Custom partition table CSV" değerine ayarlanması gerekmektedir.