/*
 * web_server.c — SoftAP-Weboberfläche + JSON-API.
 *
 *   GET  /                -> Weboberfläche (eingebettete index.html)
 *   GET  /api/devices     -> gefundene Geräte (JSON-Array)
 *   GET  /api/status      -> Scan-/Verbindungsstatus (JSON)
 *   POST /api/scan        -> Scan-Zyklus starten
 *   POST /api/connect?addr=aa:bb:..   -> verbinden (nur verbinden)
 *   POST /api/disconnect  -> trennen
 *   POST /api/beep?addr=aa:bb:..&dur=N -> 1-kHz-Ton N Sekunden senden
 */
#include "bt_tester.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "web";

/* eingebettete index.html (siehe EMBED_FILES in main/CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ---------------- Hilfen ---------------- */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1 || qlen > 256) {
        return false;
    }
    char q[256];
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
    if (txt) {
        cJSON_free(txt);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, r);
}

/* ---------------- Handler ---------------- */
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

    if (a2dp_conn_state() == CONN_CONNECTED) {
        esp_bd_addr_t peer;
        char addr[18];
        a2dp_get_peer(peer);
        bda_to_str(peer, addr);
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
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr fehlt/ungültig");
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
    char addr[18] = {0};
    char durs[8]  = {0};
    esp_bd_addr_t bda;
    if (!query_get(req, "addr", addr, sizeof(addr)) || !str_to_bda(addr, bda)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr fehlt/ungültig");
        return ESP_FAIL;
    }
    int dur = 3;
    if (query_get(req, "dur", durs, sizeof(durs))) {
        dur = atoi(durs);
    }
    if (dur < 1)  dur = 1;
    if (dur > 60) dur = 60;     /* Deckel: max. 60 s */
    a2dp_cmd_beep(bda, dur);
    return send_ok(req);
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
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server-Start fehlgeschlagen");
        return;
    }

    reg(server, "/",               HTTP_GET,  root_get);
    reg(server, "/api/devices",    HTTP_GET,  devices_get);
    reg(server, "/api/status",     HTTP_GET,  status_get);
    reg(server, "/api/scan",       HTTP_POST, scan_post);
    reg(server, "/api/connect",    HTTP_POST, connect_post);
    reg(server, "/api/disconnect", HTTP_POST, disconnect_post);
    reg(server, "/api/beep",       HTTP_POST, beep_post);

    ESP_LOGI(TAG, "Webserver läuft (http://192.168.4.1/)");
}
