# ESP32 UART Üzerinden PIN Girişi

Bu proje, ESP32'nin UART terminali üzerinden 6 haneli bir PIN kodu alınmasını sağlar.

- Sadece rakam (`0-9`) girişi kabul edilir.  
- `ENTER` tuşuna basıldığında giriş sıfırlanır.  
- `Q` karakterine basıldığında program sonlandırılır.  
- PIN girişi tam olarak 6 haneli olmalıdır, eksik ya da fazla girişler reddedilir.  
- UART olayları FreeRTOS kuyruklarıyla yönetilir.

Program, IDF terminali üzerinden kullanıcı ile etkileşime geçmek için tasarlanmıştır.