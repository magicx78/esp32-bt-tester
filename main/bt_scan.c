/*
 * bt_scan.c — Dual-Mode-Scan.
 *   - Classic-Inquiry (esp_bt_gap_start_discovery)  -> Lautsprecher/Kopfhörer
 *   - BLE-Scan        (esp_ble_gap_start_scanning)   -> nur zur Info gelistet
 * Ergebnisse werden in einer gemeinsamen, per Mutex geschützten Tabelle
 * zusammengeführt. Dieser Classic-GAP-Callback akzeptiert außerdem die
 * SSP-Bestätigung automatisch (Just Works), damit der Verbindungsaufbau der
 * A2DP-Source ohne Nutzerinteraktion klappt.
 *
 * Hinweis: Die öffentlichen Funktionen heißen bewusst bt_scan_* — ein
 * schlichtes scan_start() würde mit einem Symbol der WLAN-Bibliothek
 * (libnet80211) kollidieren.
 */
#include "bt_tester.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"

static const char *TAG = "scan";

static bt_device_t      s_dev[MAX_DEVICES];
static int              s_dev_n = 0;
static SemaphoreHandle_t s_lock;
static volatile bool    s_classic_running = false;
static volatile bool    s_ble_running     = false;

static esp_ble_scan_params_t s_ble_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ---------------- Tabelle ---------------- */
static bt_device_t *find_or_add(const esp_bd_addr_t bda, dev_type_t type)
{
    for (int i = 0; i < s_dev_n; i++) {
        if (s_dev[i].type == type && memcmp(s_dev[i].bda, bda, 6) == 0) {
            return &s_dev[i];
        }
    }
    if (s_dev_n < MAX_DEVICES) {
        bt_device_t *d = &s_dev[s_dev_n++];
        memset(d, 0, sizeof(*d));
        memcpy(d->bda, bda, 6);
        d->type = type;
        d->rssi = -127;
        return d;
    }
    return NULL;
}

int bt_scan_get_devices(bt_device_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = (s_dev_n < max) ? s_dev_n : max;
    memcpy(out, s_dev, n * sizeof(bt_device_t));
    xSemaphoreGive(s_lock);
    return n;
}

bool bt_scan_is_running(void)
{
    return s_classic_running || s_ble_running;
}

/* ---------------- Classic-GAP-Callback ---------------- */
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bt_device_t *d = find_or_add(param->disc_res.bda, DEV_TYPE_CLASSIC);
        if (d) {
            d->connectable  = true;
            d->last_seen_us = esp_timer_get_time();
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
                switch (p->type) {
                case ESP_BT_GAP_DEV_PROP_COD: {
                    d->cod = *(uint32_t *)(p->val);
                    uint32_t major = (d->cod >> 8) & 0x1F;
                    d->is_audio = (major == 0x04);   /* Audio/Video Major-Class */
                    break;
                }
                case ESP_BT_GAP_DEV_PROP_RSSI:
                    d->rssi = *(int8_t *)(p->val);
                    break;
                case ESP_BT_GAP_DEV_PROP_BDNAME: {
                    int l = p->len;
                    if (l >= DEV_NAME_LEN) l = DEV_NAME_LEN - 1;
                    memcpy(d->name, p->val, l);
                    d->name[l] = 0;
                    break;
                }
                case ESP_BT_GAP_DEV_PROP_EIR: {
                    if (d->name[0] == 0) {
                        uint8_t  nlen = 0;
                        uint8_t *name = esp_bt_gap_resolve_eir_data(
                            (uint8_t *)p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &nlen);
                        if (!name) {
                            name = esp_bt_gap_resolve_eir_data(
                                (uint8_t *)p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &nlen);
                        }
                        if (name) {
                            if (nlen >= DEV_NAME_LEN) nlen = DEV_NAME_LEN - 1;
                            memcpy(d->name, name, nlen);
                            d->name[nlen] = 0;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
        xSemaphoreGive(s_lock);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_classic_running =
            (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        ESP_LOGI(TAG, "Classic-Inquiry %s",
                 s_classic_running ? "läuft" : "beendet");
        break;

    /* --- SSP: Just Works automatisch bestätigen --- */
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP-Bestätigung -> auto-accept");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
    case ESP_BT_GAP_KEY_REQ_EVT:
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "Pairing %s",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "fehlgeschlagen");
        break;

    default:
        break;
    }
}

/* ---------------- BLE-GAP-Callback ---------------- */
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        /* Parameter gesetzt; gestartet wird explizit in bt_scan_start(). */
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_ble_running = (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bt_device_t *d = find_or_add(param->scan_rst.bda, DEV_TYPE_BLE);
            if (d) {
                d->connectable  = false;    /* BLE: nur zur Info */
                d->is_audio     = false;
                d->rssi         = param->scan_rst.rssi;
                d->last_seen_us = esp_timer_get_time();
                uint8_t  nlen = 0;
                uint8_t *name = esp_ble_resolve_adv_data(
                    param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
                if (!name) {
                    name = esp_ble_resolve_adv_data(
                        param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &nlen);
                }
                if (name && nlen > 0) {
                    if (nlen >= DEV_NAME_LEN) nlen = DEV_NAME_LEN - 1;
                    memcpy(d->name, name, nlen);
                    d->name[nlen] = 0;
                }
            }
            xSemaphoreGive(s_lock);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s_ble_running = false;
            ESP_LOGI(TAG, "BLE-Scan beendet");
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_ble_running = false;
        break;

    default:
        break;
    }
}

/* ---------------- Öffentlich ---------------- */
void bt_scan_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_cb));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_cb));

    /* SSP IO-Capability = NoInputNoOutput -> Just Works */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(
        ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t)));

    /* Verbindbar sein (als A2DP-Source-Initiator); nicht sichtbar nötig. */
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));

    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&s_ble_params));

    ESP_LOGI(TAG, "Scan-Modul bereit");
}

void bt_scan_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dev_n = 0;                 /* Tabelle für neuen Zyklus leeren */
    xSemaphoreGive(s_lock);

    s_classic_running = true;
    s_ble_running     = true;

    esp_ble_gap_start_scanning(SCAN_SECS);
    /* Inquiry-Länge 10 * 1.28 s ≈ 12.8 s, unbegrenzte Antwortanzahl */
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

    ESP_LOGI(TAG, "Scan gestartet (Classic + BLE)");
}

void bt_scan_stop(void)
{
    esp_bt_gap_cancel_discovery();
    esp_ble_gap_stop_scanning();
    s_classic_running = false;
    s_ble_running     = false;
}
