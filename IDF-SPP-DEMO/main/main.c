/*
 * ESP32 Bluetooth Classic SPP (Serial Port Profile) ve Secure Simple Pairing (SSP) Demo Projesi
 *
 * Bu demo uygulama, ESP32'nin Bluetooth Classic (BR/EDR) modunda SPP sunucusu olarak çalışmasını sağlar.
 * - Secure Simple Pairing (SSP) desteklenir, 6 haneli PIN ile güvenli eşleşme yapılır.
 * - GAP (Generic Access Profile) kullanılarak çevredeki Bluetooth cihazları taranır ve cihaz bilgileri
 *   (MAC adresi, isim, RSSI) tutulur.
 * - SPP üzerinden gelen veriler kısa mesajlar halinde loglanır.
 *
 * Önemli noktalar:
 * - BLE kullanılmaz, sadece klasik Bluetooth (BR/EDR) aktif edilir.
 * - GAP callback'leri ile cihaz tarama, isim okuma, RSSI okuma, eşleşme ve bağlantı durumları yönetilir.
 * - SPP callback'lerinde bağlantı durumu ve veri alımı işlenir.
 * - Büyük veri işleme için, gelen verileri SPP callback'indeki ESP_SPP_DATA_IND_EVT event'inde doğrudan
 *   işlemek yerine bir buffer ve FreeRTOS task kullanılması önerilir.
 */

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_bt_main.h"
#include "esp_spp_api.h"    //bluetooth classic gap api
#include "esp_bt_defs.h"    //bt tanımlamaları (enums, constants etc)
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h" //bluetooth classic gap api

#define MAX_DEVICES     10  //GAP taramasında bulunacak cihaz sınırı
#define DEVICE_NAME     "ESP32_SPP_SERVER"
#define SPP_SERVER_NAME "SPP_SERVER"

typedef struct { //Bulunan cihazların bilgilerini tutan struct
    esp_bd_addr_t bda; //bluetooth device address (struct'ın primary keyi)
    char name [ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    int rssi;
    bool name_received;
    bool rssi_received;
    bool printed_to_log;
} device_info_t;

static device_info_t found_devices[MAX_DEVICES]; //GAP taramasında bulunan cihaz dizisi
static int device_count = 0; //GAP taramasında bulunan cihaz sayısı

/* ESP_LOG tag tanımlamaları */
static const char *SCAN_TAG = "BT_SCAN";
static const char *SPP_TAG = "BT_SPP";
static const char *GAP_TAG = "BT_GAP";
static const char *PIN_TAG = "BT_SSP";
static const char *BT_TAG = "BT_INIT";

static device_info_t* find_or_add_device(esp_bd_addr_t bda) { //MAC adresi gelen cihazları bir arrayde toplamak için yardımcı fonksiyon
    for (int i = 0; i < device_count; i++) {
        if (memcmp(found_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return &found_devices[i];
        }
    }

    if (device_count < MAX_DEVICES) {
        device_info_t *device = &found_devices[device_count];
        memcpy(device->bda, bda, ESP_BD_ADDR_LEN);
        memset(device->name, 0, sizeof(device->name));
        strcpy(device->name, "Unknown");
        device->rssi = 0;
        device->name_received = false;
        device->rssi_received = false;
        device->printed_to_log = false;
        device_count++;
        return device;
    }

    return NULL;
}

static void print_device_info(device_info_t *device) {
    char bda_str[18];
    snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
            device->bda[0], device->bda[1], device->bda[2],
            device->bda[3], device->bda[4], device->bda[5]);
    
    ESP_LOGI(SCAN_TAG, "Cihaz adı: %s, MAC: %s, RSSI: %d dBm.",
            device->name, bda_str, device->rssi);
}

static char *bda2str(uint8_t * bda, char *str, size_t size) { //Bluetooth device address'leri stringe çevirmek için yardımcı fonksiyon
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) { //BT Classic GAP callback'i
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: { //cihaz bulundu
            device_info_t *device = find_or_add_device(param->disc_res.bda);

            char bda_str[18];
            bda2str(param->disc_res.bda, bda_str, 18);
            ESP_LOGW(SCAN_TAG, "Found device: %s", bda_str);
            
            if (device != NULL) {
                esp_bt_gap_read_remote_name(param->disc_res.bda); //isim alma isteğinde bulun (asenkron)
                esp_bt_gap_read_rssi_delta(param->disc_res.bda); //rssi alma isteği gönder (asenkron)
            }

            break;
        }

        case ESP_BT_GAP_READ_REMOTE_NAME_EVT: { //cihazın adı geldiyse
            device_info_t *device = find_or_add_device(param->disc_res.bda);

            if (device != NULL) {
                device->name_received = true;
                if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) { //cihazın adı okunabildiyse
                    strncpy(device->name, (char*)param->read_rmt_name.rmt_name, 
                           sizeof(device->name) - 1);
                    device->name[sizeof(device->name) - 1] = '\0';
                }
            }

            break;
        }

        case ESP_BT_GAP_READ_RSSI_DELTA_EVT: { //rssi geldiyse
            device_info_t *device = find_or_add_device(param->read_rssi_delta.bda);
            if (device != NULL) {
                device->rssi_received = true;
                if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
                    device->rssi = param->read_rssi_delta.rssi_delta;
                }
                else {
                    device->rssi = -999; // RSSI alınamadı göstergesi
                }
            }

            break;
        }

        case ESP_BT_GAP_CFM_REQ_EVT: { //MITM korumalı eşleşme onayı (6 haneli şifre)
            ESP_LOGI(PIN_TAG, "6 haneli PIN: %06"PRIu32, param->cfm_req.num_val); //PRIu32 -> Primitive sayı türlerinin boyutlarının değişkenlik gösterebildiği yerlerde kullanılır.
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true); //otomatik onaylama
            
            break;
        }

        case ESP_BT_GAP_KEY_REQ_EVT: { //kullanıcının cihazının gösterdiği şifreyi al
            ESP_LOGI(PIN_TAG, "Şifreyi girin.");
            break;
        }

        case ESP_BT_GAP_AUTH_CMPL_EVT: { //SSP eşleştirmesi tamamlandıysa
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(PIN_TAG, "Kimlik doğrulama başarılı. Cihaz adı: %s", param->auth_cmpl.device_name);
            }
            else {
                ESP_LOGI(PIN_TAG, "Kimlik doğrulama başarısız. Status: %d", param->auth_cmpl.stat);
            }

            break;
        }

        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: { //ACL bağlantısı tamamlanıysa
            if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(GAP_TAG, "ACL bağlantısı başarılı.");
            }
            else {
                ESP_LOGI(GAP_TAG, "ACL bağlantısı hata. Status: %d", param->acl_conn_cmpl_stat.stat);
            }
            
            break;
        }

        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: { //ACL bağlantısı koptuysa
            if (param->acl_disconn_cmpl_stat.reason == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(GAP_TAG, "ACL bağlantısı başarıyla durduruldu.");
            }
            else if (param->acl_disconn_cmpl_stat.reason == ESP_BT_STATUS_HCI_CONN_CAUSE_LOCAL_HOST) {
                ESP_LOGI(GAP_TAG, "ESP32 ACL bağlantısını kapattı.");
            }
            else {
                ESP_LOGI(GAP_TAG, "ACL bağlantısı kapandı. Sebep: %d", param->acl_disconn_cmpl_stat.reason);
            }
            
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: { //scan (discovery) state değişti ise
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) { //arama zamanı dolduysa
                ESP_LOGI(SCAN_TAG, "Tarama tamamlandı");

                for (int i = 0; i < device_count; i++) { //Bulunan cihazların bilgilerini yazdır
                    if (!found_devices[i].printed_to_log) {
                        if (!found_devices[i].rssi_received) {
                            found_devices[i].rssi = -999; //rssi yok
                            found_devices[i].rssi_received = true;
                            if (!found_devices[i].name_received) {
                                found_devices[i].name_received = true;
                            }
                        }
                        print_device_info(&found_devices[i]);
                        found_devices[i].printed_to_log = true;
                    }
                }

                ESP_LOGI(SCAN_TAG, "Toplam %d cihaz bulundu", device_count);
                ESP_LOGI(SCAN_TAG, "====================");
                // vTaskDelay(pdMS_TO_TICKS(3000));
                // esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 3, 0);
            }
            else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) { //arama başladıysa
                ESP_LOGI(SCAN_TAG, "Tarama başlatıldı");
            }

            break;
        }

        case ESP_BT_GAP_ENC_CHG_EVT: { //GAP event 21 (encryption changed)
            ESP_LOGI(PIN_TAG, "Şifreleme değişti.");
            break;
        }

        default: //diğer eventleri yazdır
            //ESP_LOGI(SCAN_TAG, "GAP event: %d", event);
            break;
    }
}

void bt_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) { //BT Classic SPP callback'i
    switch (event) {
        case ESP_SPP_INIT_EVT: { //spp başlatıldıysa
            if (param->init.status == ESP_SPP_SUCCESS) {
                ESP_LOGW(SPP_TAG, "SPP ilkleme başarılı. SPP sunucusu başlatılıyor.");
                esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_NAME); //Security Setting Mask, master/slave, spesifik kanal (0 = herhangi bir kanal), SPP sunucu adı
            }
            else {
                ESP_LOGE(SPP_TAG, "SPP ilkleme hatası. Status: %d", param->init.status);
            }
            break;
        }

        case ESP_SPP_CLOSE_EVT: { //spp bağlantısı kapandıysa
            ESP_LOGI(SPP_TAG, "SPP bağlantısı kapandı.");
            break;
        }

        case ESP_SPP_START_EVT: { //spp server başlatıldıysa (dinlemeye hazır)
            if (param->start.status == ESP_SPP_SUCCESS) {
                ESP_LOGW(SPP_TAG, "SPP sunucusu başlatıldı.");
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            }
            else {
                ESP_LOGE(SPP_TAG, "SPP sunucusu başlatılamadı. Status:%d", param->start.status);
            }
            break;
        }

        case ESP_SPP_DATA_IND_EVT: { //spp bağlantısından veri geldiyse
            /*
                Büyük verileri bu callback içerisinde işlemek mantıkı değildir çünkü bu callback, Bluetooth Stack'inin
                yüksek öncelikli bir parçasıdır ve büyük verileri burada işlemek bluetooth iletişimini geciktirebilir veya bozabilir.
                Bunun yerine bir buffer yapısı kullanarak gelen veriyi bir FreeRTOS taskında işlemek daha doğru olabilir.
            */
            if (param->data_ind.len < 128) {
                ESP_LOGW(SPP_TAG, "Gelen veri: %.*s, %d byte", param->data_ind.len, (char *)param->data_ind.data, param->data_ind.len);
            }
            
            break;
        }

        case ESP_SPP_SRV_OPEN_EVT: { //bir cihaz server'a bağlandıysa (veri alışverişi yapılabilir)
            ESP_LOGI(SPP_TAG, "Cihaz SPP sunucusuna bağlandı.");
            break;
        }

        case ESP_SPP_SRV_STOP_EVT: { //esp_spp_stop_srv(); çağırıldıktan sonra spp sevrer durdurulduysa
            ESP_LOGI(SPP_TAG, "ESP_SPP_SRV_STOP_EVT");
            break;
        }

        default:
            break;
    }
}

void app_main(void) {
    /* BR/EDR log seviyelerini düşür */
    esp_log_level_set("BT_HCI", ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_NONE);
    esp_log_level_set("BT_RFCOMM", ESP_LOG_ERROR);

    esp_err_t ret = nvs_flash_init(); //nvs başlat
    if (ret) {
        ESP_LOGE("MAIN", "NVS Flash ilkleme hatası: %s", esp_err_to_name(ret));
        return;
    }

    esp_bt_mem_release(ESP_BT_MODE_BLE); //ble kullanılmayacağı için memory'sini serbest bırak

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT(); //default l2cap, hci yapılandırmaları
    ret = esp_bt_controller_init(&bt_cfg); //bluetooth classic controller'ı default ayarlarla oluştur (ilkle)
    if (ret) {
        ESP_LOGE(BT_TAG, "BT Controller ilkleme hatası: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT); //bluetooth classic'i başlat
    if (ret) {
        ESP_LOGE(BT_TAG, "BT Controller aktifleştirme hatası: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init(); //bluedroid (ESP-Bluedroid is a modified version of the native Android Bluetooth stack, Bluedroid): rfcomm ve gap'in çalışmasını sağlar
    if (ret) {
        ESP_LOGE(BT_TAG, "Bluedroid ilkleme hatası: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BT_TAG, "Bluedroid aktifleştirme hatası: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bt_gap_set_device_name(DEVICE_NAME); //diğer cihazların göreceği bt ismi
    if (ret) {
        ESP_LOGE(BT_TAG, "Cihaz adı ayarlanamadı: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(BT_TAG, "BR/EDR (BT Classic) başlatıldı.");
    ESP_LOGW(BT_TAG, "Cihaz adı: %s", DEVICE_NAME);

    ret = esp_bt_gap_register_callback(bt_app_gap_cb); //bt gap callbacki bağla
    if (ret) {
        ESP_LOGE(BT_TAG, "GAP callback kaydı başarısız: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_spp_register_callback(bt_spp_cb); //bt spp callbacki bağla
    if (ret) {
        ESP_LOGE(BT_TAG, "SPP callback kaydı başarısız: %s", esp_err_to_name(ret));
        return;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB, //ESP_SPP_MODE_CB (olay tabanlı seri port) veya ESP_SPP_MODE_VFS (dosya sistemi nesnesi gibi davranır).
        .enable_l2cap_ertm = false, //Enhanced Retransmission Mode aktifliği. 
        .tx_buffer_size = 0, /* Tx buffer size for a new SPP channel. A smaller setting can save memory, but may incur a decrease in throughput. Only for ESP_SPP_MODE_VFS mode.*/
    };

    ret = esp_spp_enhanced_init(&bt_spp_cfg); //özel bir esp_spp_cfg_t ile init etme fonksiyonu. alternatif: esp_spp_init(esp_spp_mode_t mode);
    if (ret) {
        ESP_LOGE(BT_TAG, "SPP ilkleme hatası: %s", esp_err_to_name(ret));
        return;
    }

    /* BT SSP Ayarları */
    esp_bt_sp_param_t auth_req = ESP_BT_SP_IOCAP_MODE; //SSP (Secure Simple Pairing - 6 haneli pin) için default parametreler
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO; //hem giriş hem çıkış
    esp_bt_gap_set_security_param(auth_req, &iocap, sizeof(uint8_t)); //gap'e ssp korumasını ekle

    // ESP_LOGI(BT_TAG, "Bluetooth cihaz taraması başlatılıyor...");
    // esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE); //gap taraması için
    // esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0); //genel arama modunda, 10 x 1.28 sn = 12.8 sn tarama, 0 = sınırsız sayıda cihaz taraması
}