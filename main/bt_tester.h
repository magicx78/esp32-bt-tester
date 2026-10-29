/*
 * ESP32 BT-Test-Tool — gemeinsame Typen und Modul-Schnittstellen.
 * Nur für autorisierte Tests an eigenen Geräten.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_gap_bt_api.h"   /* esp_bd_addr_t */

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DEVICES   40
#define DEV_NAME_LEN  64
#define SCAN_SECS     10      /* BLE-Scandauer (s); Classic-Inquiry ~12.8s */

typedef enum {
    DEV_TYPE_CLASSIC = 0,
    DEV_TYPE_BLE     = 1,
} dev_type_t;

typedef struct {
    esp_bd_addr_t bda;              /* Geräteadresse                       */
    char          name[DEV_NAME_LEN];
    int           rssi;             /* Signalstärke (dBm)                  */
    uint32_t      cod;              /* Class of Device (nur Classic)       */
    dev_type_t    type;
    bool          is_audio;         /* Classic Audio/Video Major-Class     */
    bool          connectable;      /* Classic => true, BLE => false       */
    int64_t       last_seen_us;
} bt_device_t;

/* ---- Verbindungszustand (A2DP) ---- */
typedef enum {
    CONN_IDLE = 0,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_DISCONNECTING,
} conn_state_t;

/* ---- Scan-Modul (Classic-Inquiry + BLE-Scan) ---- */
void bt_scan_init(void);
void bt_scan_start(void);                            /* Scan-Zyklus starten          */
void bt_scan_stop(void);                             /* Scan abbrechen               */
int  bt_scan_get_devices(bt_device_t *out, int max); /* Snapshot kopieren -> Anzahl  */
bool bt_scan_is_running(void);

/* ---- A2DP-Source-Modul (Connect / Beep / Disconnect) ---- */
void         a2dp_init(void);
void         a2dp_cmd_connect(const esp_bd_addr_t bda);
void         a2dp_cmd_disconnect(void);
void         a2dp_cmd_beep(const esp_bd_addr_t bda, int duration_s);
conn_state_t a2dp_conn_state(void);
bool         a2dp_is_streaming(void);
void         a2dp_get_peer(esp_bd_addr_t out);      /* aktuelle Gegenstelle         */

/* ---- Ton-Generator (1 kHz Sinus, 44.1 kHz / 16 bit / Stereo) ---- */
int32_t tone_fill(uint8_t *buf, int32_t len);       /* füllt PCM, gibt Bytes zurück */
void    tone_reset(void);

/* ---- Webserver (SoftAP + HTTP-API + UI) ---- */
void web_start(void);

/* ---- Hilfsfunktionen ---- */
void bda_to_str(const esp_bd_addr_t bda, char *str);       /* "aa:bb:cc:dd:ee:ff" */
bool str_to_bda(const char *str, esp_bd_addr_t bda);

#ifdef __cplusplus
}
#endif
