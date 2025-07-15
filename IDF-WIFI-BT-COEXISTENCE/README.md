# ESP32 WiFi + Bluetooth Classic Coexistence Testi

Bu proje, ESP32'de WiFi (SoftAP) ile Bluetooth Classic SPP'nin aynı anda çalıştığı durumda performansını ölçmek için geliştirilmiştir.

- TCP socket üzerinden yüksek hacimli WiFi veri alımı yapılır.
- Bluetooth SPP ile yoğun veri alışverişi gerçekleştirilir.
- WiFi ve BT bağlantılarının eşzamanlı kullanımındaki paket kaybı ve bağlantı stabilitesi gözlemlenmiştir.

Çok düşük TCP bağlantısı kurulamama oranı gözlemlemiş ancak paket kaybı yaşanmamıştır.