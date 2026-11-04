/*
 * ble_gattc.c — BLE-GATT-Client ("Explorer") für autorisierte Lab-Tests/Lehre.
 *
 * Verbindet sich mit einem gewählten BLE-Peripheriegerät, entdeckt dessen
 * Dienste und Charakteristiken (mit Eigenschaften), liest und schreibt Werte.
 * Die Zugriffe laufen mit AUTH_REQ_NONE: gelingt ein Lesen/Schreiben ohne
 * Pairing/Verschlüsselung, ist das genau der didaktische Befund "ungeschützt".
 *
 * Nur gegen Geräte verwenden, die man selbst kontrolliert / die im Lab zum
 * Test freigegeben sind.
 */
#include "bt_tester.h"
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_defs.h"

static const char *TAG = "gattc";

#define GATTC_APP_ID   0
#define MAX_SVCS       24
#define OP_TIMEOUT_MS  6000
#define CONNECT_TIMEOUT_MS 10000

typedef enum { OP_NONE = 0, OP_CONNECT, OP_READ, OP_WRITE } op_t;

static esp_gatt_if_t   s_if = ESP_GATT_IF_NONE;
static uint16_t        s_conn_id;
static volatile bool   s_connected = false;
static esp_bd_addr_t   s_peer;

static SemaphoreHandle_t s_api_mtx;   /* nur eine GATT-Operation gleichzeitig */
static SemaphoreHandle_t s_done;      /* signalisiert Abschluss der Operation */
static SemaphoreHandle_t s_db_mtx;    /* schützt s_db / s_svc                 */
static volatile op_t   s_op = OP_NONE;
static volatile int    s_op_status = 0;

/* Lese-Ergebnis */
static uint8_t  s_rd_buf[64];
static int      s_rd_len = 0;

/* Dienste (temporär während der Suche) */
typedef struct { uint16_t start, end; esp_bt_uuid_t uuid; } svc_t;
static svc_t       s_svc[MAX_SVCS];
static int         s_svc_n = 0;

/* entdeckte Charakteristiken */
static gatt_char_t s_db[GATT_MAX_CHARS];
static int         s_db_n = 0;

/* ---------------- UUID -> String ---------------- */
static void uuid_to_str(const esp_bt_uuid_t *u, char *out)
{
    if (u->len == ESP_UUID_LEN_16) {
        snprintf(out, UUID_STR_LEN, "0x%04x", u->uuid.uuid16);
    } else if (u->len == ESP_UUID_LEN_32) {
        snprintf(out, UUID_STR_LEN, "0x%08x", (unsigned)u->uuid.uuid32);
    } else {
        const uint8_t *b = u->uuid.uuid128;  /* little-endian */
        snprintf(out, UUID_STR_LEN,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[15], b[14], b[13], b[12], b[11], b[10], b[9], b[8],
            b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
    }
}

/* ---------------- Charakteristiken aller Dienste einlesen ---------------- */
static void enumerate_chars(void)
{
    static esp_gattc_char_elem_t elems[16];
    xSemaphoreTake(s_db_mtx, portMAX_DELAY);
    s_db_n = 0;
    for (int i = 0; i < s_svc_n && s_db_n < GATT_MAX_CHARS; i++) {
        uint16_t count = 0;
        if (esp_ble_gattc_get_attr_count(s_if, s_conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                         s_svc[i].start, s_svc[i].end, 0, &count) != ESP_GATT_OK) {
            continue;
        }
        if (count == 0) continue;
        if (count > 16) count = 16;
        uint16_t got = count;
        if (esp_ble_gattc_get_all_char(s_if, s_conn_id, s_svc[i].start, s_svc[i].end,
                                       elems, &got, 0) != ESP_GATT_OK) {
            continue;
        }
        char svc_s[UUID_STR_LEN];
        uuid_to_str(&s_svc[i].uuid, svc_s);
        for (int c = 0; c < got && s_db_n < GATT_MAX_CHARS; c++) {
            gatt_char_t *d = &s_db[s_db_n++];
            strncpy(d->svc_uuid, svc_s, UUID_STR_LEN - 1);
            d->svc_uuid[UUID_STR_LEN - 1] = 0;
            uuid_to_str(&elems[c].uuid, d->chr_uuid);
            d->handle = elems[c].char_handle;
            d->props  = elems[c].properties;
        }
    }
    xSemaphoreGive(s_db_mtx);
    ESP_LOGI(TAG, "%d Charakteristiken in %d Diensten", s_db_n, s_svc_n);
}

/* ---------------- GATTC-Callback ---------------- */
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        s_if = gattc_if;
        ESP_LOGI(TAG, "GATTC registriert (if=%d)", gattc_if);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status == ESP_GATT_OK) {
            s_conn_id  = param->open.conn_id;
            s_connected = true;
            esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        } else {
            ESP_LOGW(TAG, "Open fehlgeschlagen, status=%d", param->open.status);
            s_op_status = -1;
            if (s_op == OP_CONNECT) xSemaphoreGive(s_done);
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        s_svc_n = 0;
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (s_svc_n < MAX_SVCS) {
            s_svc[s_svc_n].start = param->search_res.start_handle;
            s_svc[s_svc_n].end   = param->search_res.end_handle;
            s_svc[s_svc_n].uuid  = param->search_res.srvc_id.uuid;
            s_svc_n++;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        enumerate_chars();
        if (s_op == OP_CONNECT) {
            s_op_status = 0;
            xSemaphoreGive(s_done);
        }
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        if (s_op == OP_READ) {
            s_op_status = param->read.status;
            s_rd_len = 0;
            if (param->read.status == ESP_GATT_OK) {
                int n = param->read.value_len;
                if (n > (int)sizeof(s_rd_buf)) n = sizeof(s_rd_buf);
                memcpy(s_rd_buf, param->read.value, n);
                s_rd_len = n;
            }
            xSemaphoreGive(s_done);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (s_op == OP_WRITE) {
            s_op_status = param->write.status;
            xSemaphoreGive(s_done);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        s_connected = false;
        ESP_LOGI(TAG, "GATT getrennt, reason=0x%x", param->disconnect.reason);
        if (s_op != OP_NONE) {
            s_op_status = -2;
            xSemaphoreGive(s_done);
        }
        break;

    default:
        break;
    }
}

/* ---------------- Öffentliche API ---------------- */
void ble_gattc_init(void)
{
    s_api_mtx = xSemaphoreCreateMutex();
    s_db_mtx  = xSemaphoreCreateMutex();
    s_done    = xSemaphoreCreateBinary();

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(GATTC_APP_ID));
    ESP_LOGI(TAG, "BLE-GATT-Client bereit");
}

bool ble_gattc_is_connected(void) { return s_connected; }

void ble_gattc_get_peer(esp_bd_addr_t out)
{
    memcpy(out, s_peer, sizeof(esp_bd_addr_t));
}

int ble_gattc_connect(const esp_bd_addr_t bda, uint8_t addr_type)
{
    if (s_if == ESP_GATT_IF_NONE) return -10;
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);

    if (s_connected) {
        esp_ble_gattc_close(s_if, s_conn_id);
        vTaskDelay(pdMS_TO_TICKS(300));
        s_connected = false;
    }

    bt_scan_stop();                 /* Scannen stoppen (ein Funk-Core) */
    vTaskDelay(pdMS_TO_TICKS(150));

    memcpy(s_peer, bda, sizeof(esp_bd_addr_t));
    xSemaphoreTake(s_done, 0);      /* Semaphore leeren */
    s_op = OP_CONNECT;
    s_op_status = -1;

    esp_err_t err = esp_ble_gattc_open(s_if, (uint8_t *)bda,
                                       (esp_ble_addr_type_t)addr_type, true);
    int rc = -1;
    if (err == ESP_OK &&
        xSemaphoreTake(s_done, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) == pdTRUE) {
        rc = s_op_status;           /* 0 = ok */
    } else {
        ESP_LOGW(TAG, "Connect-Timeout/Fehler (err=%d)", err);
        rc = -1;
    }
    s_op = OP_NONE;
    xSemaphoreGive(s_api_mtx);
    return rc;
}

void ble_gattc_disconnect(void)
{
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);
    if (s_connected) {
        esp_ble_gattc_close(s_if, s_conn_id);
    }
    s_op = OP_NONE;
    xSemaphoreGive(s_api_mtx);
}

int ble_gattc_get_db(gatt_char_t *out, int max)
{
    xSemaphoreTake(s_db_mtx, portMAX_DELAY);
    int n = (s_db_n < max) ? s_db_n : max;
    memcpy(out, s_db, n * sizeof(gatt_char_t));
    xSemaphoreGive(s_db_mtx);
    return n;
}

int ble_gattc_read(uint16_t handle, uint8_t *out, int max, int *gatt_status)
{
    if (!s_connected) return -1;
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);
    xSemaphoreTake(s_done, 0);
    s_op = OP_READ;
    s_op_status = -1;

    int rc = -1;
    esp_err_t err = esp_ble_gattc_read_char(s_if, s_conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
    if (err == ESP_OK &&
        xSemaphoreTake(s_done, pdMS_TO_TICKS(OP_TIMEOUT_MS)) == pdTRUE) {
        if (gatt_status) *gatt_status = s_op_status;
        if (s_op_status == ESP_GATT_OK) {
            int n = (s_rd_len < max) ? s_rd_len : max;
            memcpy(out, s_rd_buf, n);
            rc = n;
        } else {
            rc = -1;                /* z.B. insufficient auth/encryption */
        }
    } else if (gatt_status) {
        *gatt_status = -1;
    }
    s_op = OP_NONE;
    xSemaphoreGive(s_api_mtx);
    return rc;
}

int ble_gattc_write(uint16_t handle, const uint8_t *data, int len,
                    bool with_response, int *gatt_status)
{
    if (!s_connected) return -1;
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);
    xSemaphoreTake(s_done, 0);
    s_op = OP_WRITE;
    s_op_status = -1;

    esp_gatt_write_type_t wt = with_response ? ESP_GATT_WRITE_TYPE_RSP
                                             : ESP_GATT_WRITE_TYPE_NO_RSP;
    int rc = -1;
    esp_err_t err = esp_ble_gattc_write_char(s_if, s_conn_id, handle, len,
                                             (uint8_t *)data, wt, ESP_GATT_AUTH_REQ_NONE);
    if (!with_response) {
        /* Write-ohne-Antwort erzeugt kein WRITE_CHAR_EVT */
        if (gatt_status) *gatt_status = (err == ESP_OK) ? 0 : -1;
        rc = (err == ESP_OK) ? 0 : -1;
    } else if (err == ESP_OK &&
               xSemaphoreTake(s_done, pdMS_TO_TICKS(OP_TIMEOUT_MS)) == pdTRUE) {
        if (gatt_status) *gatt_status = s_op_status;
        rc = (s_op_status == ESP_GATT_OK) ? 0 : -1;
    } else if (gatt_status) {
        *gatt_status = -1;
    }
    s_op = OP_NONE;
    xSemaphoreGive(s_api_mtx);
    return rc;
}
