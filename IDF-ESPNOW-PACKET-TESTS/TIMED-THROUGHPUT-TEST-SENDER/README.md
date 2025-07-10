# TIMED_THROUGHPUT_TEST

## Amaç

Bu testin amacı, ESP32 cihazları arasında ESP-NOW protokolü kullanılarak yapılan sabit süreli veri iletiminde sistemin veri aktarım performansını (throughput) ölçmek ve buna etki eden parametreleri analiz etmektir.

Test kapsamında, bir gönderici ESP32 cihazı tarafından belirli uzunlukta veri paketleri sabit süre boyunca bir alıcı ESP32 cihazına iletilmiş, ardından gönderici ve alıcı tarafından ölçülen veri miktarları karşılaştırılarak sistemin genel aktarım hızı ve güvenilirliği değerlendirilmiştir.

## Test Donanımı

- **Gönderici**: ESP32-DevKitC V4 (ESP32-WROOM-32UE)
- **Alıcı**: ESP32-S3-DevKitC-1 (ESP32-S3-WROOM-1)
- **İletim Protokolü**: ESP-NOW V2 (ESP-IDF v5.4.1 ile)

## Test Parametreleri

- **Veri Paketi Boyutu**: 1024 byte (sabit)
- **Toplam Test Süresi**: 10 saniye (alıcı tarafında +0.05 saniye tolere süresi)
- **İletim Yönü**: Tek yönlü (Gönderici → Alıcı)
- **ESP-NOW Şifreleme**: Devre dışı
- **ACK Mekanizması**: Başlangıçta senkronizasyon için basit ACK (1 byte)

## Gözlemler

1. **Ortalama Throughput**:  
   - Verici tarafında ölçülen ortalama throughput: **95–100 KB/s**
   - Alıcı tarafında bu değer +0.05 saniye tolere süresinden dolayı göz ardı edilebilecek bir oranda daha düşük çıkmaktadır.

2. **Test Süresinin Etkisi**:  
   - Süre uzadıkça ölçüm sonuçları daha stabil ve gerçekçi hale gelmektedir. Kısa süreli testlerde anlık darboğazlar ölçümü negatif etkileyebilir.

3. **Zamanlama Uyumu**:  
   - Cihazlar arası zamanlama doğru yapılandırıldığında (özellikle alıcı tarafındaki dinleme süresi), **hiçbir paket kaybı yaşanmamıştır**.

4. **Gönderim Arasındaki Bekleme Süresi (Delay)**:  
   - `esp_now_send()` çağrıları arasındaki gecikme süresi **milisaniyelerden mikrosaniyelere indirildiğinde throughput üzerinde anlamlı bir etki gözlemlenmemiştir**.
   - Delay süresini tamamen kaldırmak **Watchdog Timer (WDT)** tetiklenmesine ve kullanıcının terminal üzerinden uyarılmasına neden olmaktadır.
   - Denemeler süresince en uygun gecikme süresi olarak **2 ms** tecrübe edilmiştir. Bu değer sistemin kararlılığını bozmazken en yüksek throughput'u sağlamaktadır.

5. **Dinleme Süresinin Uzatılması**:  
   - Alıcı ESP32 tarafında dinleme süresi göndericiye göre **0.05 saniye daha uzun tutulduğunda**, zamanlama kaynaklı potansiyel paket kayıpları sıfıra indirgenmiştir.
   - Bu yöntem throughput değerinde sadece **1‰ (1/1000)** kadar küçük bir azalmaya neden olmuştur, ancak güvenilirliği artırdığı için tercih edilmiştir.

6. **Cihaz Türüne Bağlı Performans**:
   - Örnek senaryoda alıcı olan ESP32-S3-DevKitC-1, verici rolüne getirildiğinde throughput değerinde yaklaşık **%5**'lik bir artış yaşandığı ve ortalama throughput değerinin **100–105 KB/s** seviyelerine geldiği gözlemlenmiştir.

## Sonuç

Bu test, ESP-NOW protokolünün iki ESP32 cihazı arasında yüksek hızlı, düşük gecikmeli veri iletimine olanak tanıdığını ve uygun zamanlama, delay optimizasyonu ve sistem yükü yönetimi ile cihaz türüne bağlı olarak **95–105 KB/s** seviyelerinde kararlı bir throughput elde edilebileceğini göstermiştir. 

Ayrıca sistem kaynaklarının (özellikle WDT ve görev zamanlaması) dikkatli yönetilmesi durumunda, paket kaybı olmadan stabil bir iletim gerçekleştirmek mümkündür.

## Ek Bilgi

- Gönderici ve alıcı cihazlar arasında sabit kanal (Channel 1) kullanılmıştır.
- Tüm testler doğrudan ESP-IDF ortamında geliştirilmiş, SPIFFS, Wi-Fi TCP/IP stack veya ek protokoller kullanılmamıştır.
- Bu test yalnızca ESP-NOW üzerinden tek yönlü aktarımı baz alır. Çift yönlü ya da broadcast testleri bu kapsamın dışındadır.