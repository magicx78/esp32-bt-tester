/*
 * web_server.c — SoftAP-Weboberfläche + JSON-API.
 *
 * A2DP (Classic):
 *   GET  /                 -> Weboberfläche (eingebettete index.html)
 *   GET  /api/devices      -> gefundene Geräte (JSON-Array)
 *   GET  /api/status       -> Scan-/Verbindungsstatus (JSON)
 *   POST /api/scan         -> Scan-Zyklus starten
 *   POST /api/connect?addr= -> A2DP verbinden (nur verbinden)
 *   POST /api/disconnect   -> A2DP trennen
 *   POST /api/beep?addr=&dur=N -> 1-kHz-Ton N Sekunden senden
 *
 * BLE-GATT-Explorer (nur fuer autorisierte Lab-Tests):
 *   POST /api/gatt/connect?addr=   -> verbinden + Dienste/Charakteristiken entdecken
 *   POST /api/gatt/disconnect      -> GATT trennen
 *   GET  /api/gatt/db              -> entdeckte Charakteristiken (JSON)
 *   POST /api/gatt/read?handle=N   -> Charakteristik lesen
 *   POST /api/gatt/write?handle=N&hex=..|text=..[&resp=0|1] -> schreiben
 */
#include "bt_tester.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_gatt_defs.h"
#include "cJSON.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ---------------- Hilfen ---------------- */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1 || qlen > 512) {
        return false;
    }
    char q[512];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(q, key, out, out_len) == ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, txt ? txt : "{}");
    if (txt) cJSON_free(txt);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, r);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "a1b2" -> bytes; gibt Länge zurück oder -1 bei Fehler */
static int hex_decode(const char *hex, uint8_t *out, int max)
{
    int n = strlen(hex);
    if (n % 2 != 0) return -1;
    int bytes = n / 2;
    if (bytes > max) return -1;
    for (int i = 0; i < bytes; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return bytes;
}

static void to_hex(const uint8_t *in, int n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i]     = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 0xf];
    }
    out[2 * n] = 0;
}

/* ---------------- A2DP-Handler ---------------- */
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t devices_get(httpd_req_t *req)
{
    static bt_device_t devs[MAX_DEVICES];
    int n = bt_scan_get_devices(devs, MAX_DEVICES);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", bt_scan_is_running());
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");

    char addr[18];
    for (int i = 0; i < n; i++) {
        bda_to_str(devs[i].bda, addr);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", devs[i].name[0] ? devs[i].name : "(ohne Name)");
        cJSON_AddStringToObject(o, "addr", addr);
        cJSON_AddStringToObject(o, "type",
                                devs[i].type == DEV_TYPE_CLASSIC ? "classic" : "ble");
        cJSON_AddNumberToObject(o, "rssi", devs[i].rssi);
        cJSON_AddBoolToObject(o, "audio", devs[i].is_audio);
        cJSON_AddBoolToObject(o, "connectable", devs[i].connectable);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static const char *state_str(conn_state_t s)
{
    switch (s) {
    case CONN_CONNECTING:    return "connecting";
    case CONN_CONNECTED:     return "connected";
    case CONN_DISCONNECTING: return "disconnecting";
    default:                 return "idle";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", bt_scan_is_running());
    cJSON_AddStringToObject(root, "state", state_str(a2dp_conn_state()));
    cJSON_AddBoolToObject(root, "streaming", a2dp_is_streaming());
    cJSON_AddBoolToObject(root, "gatt_connected", ble_gattc_is_connected());

    if (a2dp_conn_state() == CONN_CONNECTED) {
        esp_bd_addr_t peer; char addr[18];
        a2dp_get_peer(peer); bda_to_str(peer, addr);
        cJSON_AddStringToObject(root, "peer", addr);
    } else {
        cJSON_AddStringToObject(root, "peer", "");
    }
    return send_json(req, root);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    bt_scan_start();
    return send_ok(req);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char addr[18] = {0};
    esp_bd_addr_t bda;
    if (!query_get(req, "addr", addr, sizeof(addr)) || !str_to_bda(addr, bda)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr fehlt/ungueltig");
        return ESP_FAIL;
    }
    a2dp_cmd_connect(bda);
    return send_ok(req);
}

static esp_err_t disconnect_post(httpd_req_t *req)
{
    a2dp_cmd_disconnect();
    return send_ok(req);
}

static esp_err_t beep_post(httpd_req_t *req)
{
    char addr[18] = {0}, durs[8] = {0};
    esp_bd_addr_t bda;
    if (!query_get(req, "addr", addr, sizeof(addr)) || !str_to_bda(addr, bda)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr fehlt/ungueltig");
        return ESP_FAIL;
    }
    int dur = 3;
    if (query_get(req, "dur", durs, sizeof(durs))) dur = atoi(durs);
    if (dur < 1)  dur = 1;
    if (dur > 60) dur = 60;
    a2dp_cmd_beep(bda, dur);
    return send_ok(req);
}

/* ---------------- BLE-GATT-Handler ---------------- */
static esp_err_t gatt_connect_post(httpd_req_t *req)
{
    char addr[18] = {0};
    esp_bd_addr_t bda;
    if (!query_get(req, "addr", addr, sizeof(addr)) || !str_to_bda(addr, bda)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr fehlt/ungueltig");
        return ESP_FAIL;
    }
    uint8_t at = bt_scan_ble_addr_type(bda);
    int rc = ble_gattc_connect(bda, at);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", rc == 0);
    cJSON_AddNumberToObject(r, "rc", rc);
    return send_json(req, r);
}

static esp_err_t gatt_disconnect_post(httpd_req_t *req)
{
    ble_gattc_disconnect();
    return send_ok(req);
}

static esp_err_t gatt_db_get(httpd_req_t *req)
{
    static gatt_char_t chars[GATT_MAX_CHARS];
    int n = ble_gattc_get_db(chars, GATT_MAX_CHARS);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", ble_gattc_is_connected());
    cJSON *arr = cJSON_AddArrayToObject(root, "chars");
    for (int i = 0; i < n; i++) {
        uint8_t p = chars[i].props;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "svc", chars[i].svc_uuid);
        cJSON_AddStringToObject(o, "uuid", chars[i].chr_uuid);
        cJSON_AddNumberToObject(o, "handle", chars[i].handle);
        cJSON_AddBoolToObject(o, "read",     (p & ESP_GATT_CHAR_PROP_BIT_READ)     != 0);
        cJSON_AddBoolToObject(o, "write",    (p & ESP_GATT_CHAR_PROP_BIT_WRITE)    != 0);
        cJSON_AddBoolToObject(o, "write_nr", (p & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0);
        cJSON_AddBoolToObject(o, "notify",   (p & ESP_GATT_CHAR_PROP_BIT_NOTIFY)   != 0);
        cJSON_AddBoolToObject(o, "indicate", (p & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static esp_err_t gatt_read_post(httpd_req_t *req)
{
    char hs[12] = {0};
    if (!query_get(req, "handle", hs, sizeof(hs))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "handle fehlt");
        return ESP_FAIL;
    }
    uint16_t handle = (uint16_t)atoi(hs);
    uint8_t buf[64];
    int status = -1;
    int len = ble_gattc_read(handle, buf, sizeof(buf), &status);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", len >= 0);
    cJSON_AddNumberToObject(r, "status", status);   /* 0 = ok; sonst GATT-Fehler */
    if (len >= 0) {
        char hex[64 * 2 + 1];
        to_hex(buf, len, hex);
        cJSON_AddStringToObject(r, "hex", hex);
        char ascii[65];
        int a = 0;
        for (int i = 0; i < len && a < 64; i++) {
            ascii[a++] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
        }
        ascii[a] = 0;
        cJSON_AddStringToObject(r, "ascii", ascii);
        cJSON_AddNumberToObject(r, "len", len);
    }
    return send_json(req, r);
}

static esp_err_t gatt_write_post(httpd_req_t *req)
{
    char hs[12] = {0}, hex[130] = {0}, txt[65] = {0}, resp[4] = {0};
    if (!query_get(req, "handle", hs, sizeof(hs))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "handle fehlt");
        return ESP_FAIL;
    }
    uint16_t handle = (uint16_t)atoi(hs);

    uint8_t data[64];
    int len = -1;
    if (query_get(req, "hex", hex, sizeof(hex))) {
        len = hex_decode(hex, data, sizeof(data));
    } else if (query_get(req, "text", txt, sizeof(txt))) {
        len = strlen(txt);
        if (len > (int)sizeof(data)) len = sizeof(data);
        memcpy(data, txt, len);
    }
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hex/text fehlt/ungueltig");
        return ESP_FAIL;
    }
    bool with_resp = true;
    if (query_get(req, "resp", resp, sizeof(resp)) && resp[0] == '0') with_resp = false;

    int status = -1;
    int rc = ble_gattc_write(handle, data, len, with_resp, &status);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", rc == 0);
    cJSON_AddNumberToObject(r, "status", status);
    return send_json(req, r);
}

/* ---------------- Registrierung ---------------- */
static void reg(httpd_handle_t s, const char *uri, httpd_method_t m,
                esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h };
    httpd_register_uri_handler(s, &u);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server-Start fehlgeschlagen");
        return;
    }

    reg(server, "/",                HTTP_GET,  root_get);
    reg(server, "/api/devices",     HTTP_GET,  devices_get);
    reg(server, "/api/status",      HTTP_GET,  status_get);
    reg(server, "/api/scan",        HTTP_POST, scan_post);
    reg(server, "/api/connect",     HTTP_POST, connect_post);
    reg(server, "/api/disconnect",  HTTP_POST, disconnect_post);
    reg(server, "/api/beep",        HTTP_POST, beep_post);

    reg(server, "/api/gatt/connect",    HTTP_POST, gatt_connect_post);
    reg(server, "/api/gatt/disconnect", HTTP_POST, gatt_disconnect_post);
    reg(server, "/api/gatt/db",         HTTP_GET,  gatt_db_get);
    reg(server, "/api/gatt/read",       HTTP_POST, gatt_read_post);
    reg(server, "/api/gatt/write",      HTTP_POST, gatt_write_post);

    ESP_LOGI(TAG, "Webserver laeuft (http://192.168.4.1/)");
}
