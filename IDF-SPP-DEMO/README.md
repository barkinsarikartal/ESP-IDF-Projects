# ESP32 Bluetooth Classic SPP + SSP Demo

Bu proje, ESP32 üzerinde çalışan bir Bluetooth Classic (BR/EDR) uygulamasıdır. Cihaz, SPP (Serial Port Profile) üzerinden veri alışverişi yaparken çevredeki Bluetooth cihazlarını tarar ve güvenli eşleşme (SSP - Secure Simple Pairing) destekler.

- SSP (Secure Simple Pairing) desteği ile 6 haneli PIN üzerinden güvenli eşleşme yapılır.

- GAP taraması ile çevredeki Bluetooth cihazlarının:

        - MAC adresi
        - Cihaz ismi
        - RSSI bilgileri

    okunur ve terminale yazdırılır.

- SPP (Serial Port Profile) ile gelen kısa veriler (128 byte'a kadar) loglanır.

## NOT:
Bu örnek, büyük veri transferleri için optimize edilmemiştir. Bu örnekte gelen veriler (128 byte'a kadar) SPP callback içerisinde işlenmiştir.

Ancak, büyük verileri bu callback içerisinde işlemek mantıkı değildir çünkü bu callback, Bluetooth Stack'inin yüksek öncelikli bir parçasıdır ve büyük verileri burada işlemek bluetooth iletişimini geciktirebilir veya bozabilir.

Bunun yerine bir buffer yapısı kullanarak gelen veriyi bir FreeRTOS taskında işlemek daha doğru olabilir.