/*
 * a2dp_src.c — A2DP-Source: mit einem Gerät verbinden und einen 1-kHz-Ton
 * für eine wählbare Dauer streamen. Befehle kommen über eine FreeRTOS-Queue
 * von den Web-Handlern; die eigentliche BT-Logik läuft ereignisgesteuert im
 * Bluedroid-Callback plus einem kleinen Steuer-Task.
 */
#include "bt_tester.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"

static const char *TAG = "a2dp";

typedef enum { CMD_CONNECT, CMD_DISCONNECT, CMD_BEEP } a2dp_cmd_type_t;

typedef struct {
    a2dp_cmd_type_t type;
    esp_bd_addr_t   bda;
    int             duration_s;
} a2dp_cmd_t;

static QueueHandle_t      s_cmd_q;
static SemaphoreHandle_t  s_lock;
static esp_timer_handle_t s_beep_timer;

static esp_bd_addr_t      s_peer;
static volatile conn_state_t s_conn        = CONN_IDLE;
static volatile bool      s_streaming      = false;
static volatile bool      s_want_beep      = false;   /* nach Connect Ton starten */
static volatile int       s_beep_seconds   = 3;

/* ---------------- Status-Getter ---------------- */
conn_state_t a2dp_conn_state(void) { return s_conn; }
bool         a2dp_is_streaming(void) { return s_streaming; }

void a2dp_get_peer(esp_bd_addr_t out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_peer, sizeof(esp_bd_addr_t));
    xSemaphoreGive(s_lock);
}

static void set_peer(const esp_bd_addr_t bda)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_peer, bda, sizeof(esp_bd_addr_t));
    xSemaphoreGive(s_lock);
}

/* ---------------- Dauer-Timer: Ton nach Ablauf stoppen ---------------- */
static void beep_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Piepton-Dauer abgelaufen -> SUSPEND");
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
}

/* ---------------- A2DP Daten-Callback (liefert PCM-Ton) ----------------
 * Wird nur aufgerufen, während der Mediastream läuft (also während des
 * Pieptons). Füllt den Puffer mit dem 1-kHz-Sinus.
 */
static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    if (len < 0 || buf == NULL) {
        return 0;
    }
    return tone_fill(buf, len);
}

/* ---------------- A2DP Ereignis-Callback ---------------- */
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            set_peer(param->conn_stat.remote_bda);
            s_conn = CONN_CONNECTED;
            ESP_LOGI(TAG, "A2DP verbunden");
            if (s_want_beep) {
                tone_reset();
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            s_conn      = CONN_IDLE;
            s_streaming = false;
            s_want_beep = false;
            esp_timer_stop(s_beep_timer);
            ESP_LOGI(TAG, "A2DP getrennt");
            break;
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            s_conn = CONN_CONNECTING;
            break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            s_conn = CONN_DISCONNECTING;
            break;
        default:
            break;
        }
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t cmd    = param->media_ctrl_stat.cmd;
        esp_a2d_media_ctrl_ack_t st = param->media_ctrl_stat.status;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            st  == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND ||
                   cmd == ESP_A2D_MEDIA_CTRL_STOP) {
            s_streaming = false;
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_streaming = true;
            s_want_beep = false;
            int secs = s_beep_seconds;
            esp_timer_stop(s_beep_timer);
            esp_timer_start_once(s_beep_timer, (uint64_t)secs * 1000000ULL);
            ESP_LOGI(TAG, "Audio gestartet (%d s)", secs);
        } else {
            s_streaming = false;
        }
        break;
    }

    default:
        break;
    }
}

/* ---------------- Steuer-Task: verarbeitet Web-Befehle ---------------- */
static void a2dp_task(void *arg)
{
    a2dp_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Vor jedem Verbindungsaufbau den Scan stoppen (ein Funk-Core). */
        switch (cmd.type) {
        case CMD_CONNECT:
            bt_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            s_want_beep = false;
            set_peer(cmd.bda);
            s_conn = CONN_CONNECTING;
            ESP_LOGI(TAG, "Verbinde ...");
            esp_a2d_source_connect(cmd.bda);
            break;

        case CMD_BEEP:
            bt_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            s_beep_seconds = (cmd.duration_s > 0) ? cmd.duration_s : 3;
            if (s_conn == CONN_CONNECTED && memcmp(s_peer, cmd.bda, 6) == 0) {
                /* bereits mit dem Ziel verbunden -> direkt starten */
                s_want_beep = false;
                tone_reset();
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            } else {
                /* ggf. alte Verbindung trennen, dann neu verbinden + Ton */
                if (s_conn == CONN_CONNECTED) {
                    esp_a2d_source_disconnect(s_peer);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                s_want_beep = true;
                set_peer(cmd.bda);
                s_conn = CONN_CONNECTING;
                ESP_LOGI(TAG, "Verbinde für Piepton ...");
                esp_a2d_source_connect(cmd.bda);
            }
            break;

        case CMD_DISCONNECT:
            if (s_conn == CONN_CONNECTED || s_conn == CONN_CONNECTING) {
                ESP_LOGI(TAG, "Trenne ...");
                esp_a2d_source_disconnect(s_peer);
            }
            break;
        }
    }
}

/* ---------------- Öffentliche Befehle (von den Web-Handlern) ---------------- */
void a2dp_cmd_connect(const esp_bd_addr_t bda)
{
    a2dp_cmd_t c = { .type = CMD_CONNECT, .duration_s = 0 };
    memcpy(c.bda, bda, sizeof(esp_bd_addr_t));
    xQueueSend(s_cmd_q, &c, 0);
}

void a2dp_cmd_disconnect(void)
{
    a2dp_cmd_t c = { .type = CMD_DISCONNECT, .duration_s = 0 };
    xQueueSend(s_cmd_q, &c, 0);
}

void a2dp_cmd_beep(const esp_bd_addr_t bda, int duration_s)
{
    a2dp_cmd_t c = { .type = CMD_BEEP, .duration_s = duration_s };
    memcpy(c.bda, bda, sizeof(esp_bd_addr_t));
    xQueueSend(s_cmd_q, &c, 0);
}

/* ---------------- Init ---------------- */
void a2dp_init(void)
{
    s_lock  = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(a2dp_cmd_t));

    const esp_timer_create_args_t targs = {
        .callback = beep_timer_cb,
        .name     = "beep",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_beep_timer));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    xTaskCreate(a2dp_task, "a2dp_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "A2DP-Source bereit");
}
