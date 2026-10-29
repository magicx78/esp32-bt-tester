/*
 * tone_gen.c — 1 kHz Sinuston für die A2DP-Source.
 *
 * Die A2DP-Source erwartet PCM mit 44100 Hz, 16 bit, 2 Kanäle (interleaved L/R).
 * Der SBC-Encoder im Bluedroid-Stack kodiert das Ergebnis für die Gegenstelle.
 */
#include "bt_tester.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE   44100
#define TONE_FREQ     1000.0f   /* fester Piepton: 1 kHz            */
#define TONE_AMPL     13000     /* ~0.4 Vollausschlag (nicht zu laut) */

static float s_phase = 0.0f;

void tone_reset(void)
{
    s_phase = 0.0f;
}

int32_t tone_fill(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        return 0;
    }

    const int frames = len / 4;            /* 2 Kanäle * 2 Byte = 4 Byte/Frame */
    int16_t  *out    = (int16_t *)buf;
    const float inc  = 2.0f * (float)M_PI * TONE_FREQ / (float)SAMPLE_RATE;

    for (int i = 0; i < frames; i++) {
        int16_t s = (int16_t)(TONE_AMPL * sinf(s_phase));
        s_phase += inc;
        if (s_phase >= 2.0f * (float)M_PI) {
            s_phase -= 2.0f * (float)M_PI;
        }
        out[2 * i]     = s;   /* links  */
        out[2 * i + 1] = s;   /* rechts */
    }

    return frames * 4;
}
