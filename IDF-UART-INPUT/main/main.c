/**
 * @brief UART üzerinden terminal aracılığıyla 6 haneli PIN girişi alınmasını sağlayan ESP-IDF uygulaması.
 *
 * Bu program, ESP32'nin UART0 portunu kullanarak kullanıcıdan 6 basamaklı bir PIN kodu alır.
 * Kullanıcı yalnızca rakam girebilir, hatalı girişlerde sıfırlama yapılır, ENTER tuşu ile girdi sıfırlanabilir ve özel karakterle (Q) çıkış yapılabilir.
 * Kod, ESP-IDF'in FreeRTOS altyapısı ile çalışır ve UART olaylarını dinleyen bir task içerir.
 */

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#define UART_PORT_NUM   UART_NUM_0
#define UART_BAUD_RATE  115200
#define UART_TX_PIN     GPIO_NUM_17
#define UART_RX_PIN     GPIO_NUM_16
#define UART_BUF_SIZE   1024
#define PIN_LENGTH      6

static const char *UART_TAG = "UART";

static QueueHandle_t uart0_queue;

void uart_init() {
    vTaskDelay(100 / portTICK_PERIOD_MS); //terminal çıktılarında stabilite için

    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE, //baud rate bit/saniye (karşıdaki cihazda da aynı olmalı)
        .data_bits = UART_DATA_8_BITS, //bir uart frame'inin veri biti sayısı
        .parity = UART_PARITY_DISABLE, //parite biti: veri aktarımında hata kontrolü
        .stop_bits = UART_STOP_BITS_1, //uart stop bit (her çerçevenin sonunda gönderilen dur biti sayısı)
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, //uart donanımsal akış kontrol modu (cts (clear to send) / rts (request to send))
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 20, &uart0_queue, 0); //port numarası, rx buffer boyutu, tx buffer boyutu, uart olaylarını dinleyen freertos queue boyutu, uart queue, interrupt flags
    uart_param_config(UART_PORT_NUM, &uart_config); //port numarası, uart config
    uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); //port numarası, tx pin, rx pin, rts pin, cts pin (IDF MONİTÖRÜNDEN HABERLEŞME İÇİN TX VE RX PİNLERİ KESİNLİKLE UART_PIN_NO_CHANGE OLMALI!)
    ESP_LOGW(UART_TAG, "UART0 driver kurulumu tamamlandı.");
}

void uart_read_task() {
    ESP_LOGW(UART_TAG, "UART Read Task Started");
    vTaskDelay(500 / portTICK_PERIOD_MS);

    uart_event_t event;

    uint8_t digit_buf[PIN_LENGTH + 1]; //6 basamak + null terminator
    int digit_count = 0;

    printf("\n0-9 arası rakam içeren 6 haneli sayı giriniz.");
    printf("\nYanlış tuşladığınızda girdiyi iptal etmek için -> ENTER");
    printf("\nÇıkış yapmak için -> q");
    printf("\n>> ");
    fflush(stdout);

    while (1) {
        if (xQueueReceive(uart0_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA: {
                    uint8_t tmp[PIN_LENGTH + 1];
                    int len = uart_read_bytes(UART_PORT_NUM, tmp, event.size, portMAX_DELAY); //\0 gelene kadar değil anlık okuyor
                    for (int i = 0; i < len; i++) {
                        char ch = tmp[i];
                        printf("%c", ch);
                        fflush(stdout); //karakter geldikçe yazmasını sağlar

                        if (ch == '\r' || ch == '\n') { //enter basıldıysa (cr, lf, crlf) inputu sıfırla
                            printf("\nGiriş sıfırlandı.\n>> ");
                            fflush(stdout);
                            digit_count = 0;
                            continue; //diğer if bloklarına girme
                        }

                        if (ch == 'q' || ch == 'Q') { //çıkış karakteri
                            printf("\nTerminal kapatılıyor...\n");
                            vTaskDelay(1200 / portTICK_PERIOD_MS);
                            ESP_LOGW(UART_TAG, "UART Read Task Terminated");
                            vTaskDelete(NULL);
                        }

                        if (ch >= '0' && ch <= '9') { //sadece rakam kabul et
                            if (digit_count < PIN_LENGTH) {
                                digit_buf[digit_count++] = ch;
                            }
                            if (digit_count == PIN_LENGTH) {
                                digit_buf[PIN_LENGTH] = '\0';
                                printf("\nGirilen sayı: %s\n>> ", digit_buf);
                                fflush(stdout);
                                memset(digit_buf, 0, PIN_LENGTH + 1); //sonraki input için sıfırla
                                digit_count = 0;
                            }
                        }
                        else { //0-9 olmayan bir karakter
                            printf(" --- Geçersiz karakter.\nLütfen sadece 0-9 girin.\n>> ");
                            fflush(stdout);
                            digit_count = 0;
                        }
                    }
                    break;
                }
                
                default:
                    ESP_LOGW(UART_TAG, "event.type = (%d)", event.type);
                    break;
            }
        }
    }
    ESP_LOGI(UART_TAG, "deleting uart task.");
    vTaskDelete(NULL); //FreeRTOS task'ları return etmemeli. Task bittiği zaman bu komut ile silinmeli.
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret) {
        ESP_LOGE("MAIN", "NVS Flash ilkleme hatası: %s", esp_err_to_name(ret));
        return;
    }

    esp_log_level_set("uart", ESP_LOG_WARN);
    esp_log_level_set(UART_TAG, ESP_LOG_WARN);

    if (uart_is_driver_installed(UART_NUM_0)) {
        ESP_LOGW(UART_TAG, "UART0 driver zaten kurulu");
    }
    else{
        ESP_LOGW(UART_TAG, "UART0 driver kurulu değil");
        uart_init();
    }

    xTaskCreate(uart_read_task, "uart_read_task", 4096, NULL, 5, NULL); //task, task adı, stack derinliği, parametreleri, priority, handle
}