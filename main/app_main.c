/*
 * app_main.c — Initialisierung:
 *   NVS -> WLAN (SoftAP) -> Bluetooth (Dual-Mode) -> Scan + A2DP + Webserver.
 *
 * Nur für autorisierte Tests an eigenen Geräten.
 */
#include "bt_tester.h"
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

static const char *TAG = "main";

#define AP_SSID  "ESP32-BT-Tester"
#define AP_PASS  "bttester123"     /* >= 8 Zeichen (WPA2) */

/* ---------------- Hilfsfunktionen (in bt_tester.h deklariert) ---------------- */
void bda_to_str(const esp_bd_addr_t bda, char *str)
{
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

bool str_to_bda(const char *s, esp_bd_addr_t bda)
{
    unsigned int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)v[i];
    }
    return true;
}

/* ---------------- WLAN SoftAP ---------------- */
static void wifi_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len      = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel       = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode      = WIFI_AUTH_WPA2_PSK;
    if (strlen(AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP '%s' gestartet (IP 192.168.4.1)", AP_SSID);
}

/* ---------------- Bluetooth Dual-Mode ---------------- */
static void bt_init(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    esp_bt_dev_set_device_name(AP_SSID);
    ESP_LOGI(TAG, "Bluetooth (Dual-Mode) aktiv");
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_softap();      /* WLAN zuerst (Koexistenz) */
    bt_init();

    bt_scan_init();
    a2dp_init();
    web_start();

    ESP_LOGI(TAG, "Bereit. WLAN '%s' verbinden, dann http://192.168.4.1/ öffnen.", AP_SSID);
}
