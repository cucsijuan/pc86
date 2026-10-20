/*
 * vga86 - monitors. See include/monitor.h.
 */
#include "monitor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PIXELS (VGA_OUT_WIDTH * VGA_OUT_HEIGHT)

struct monitor {
    monitor_model_t model;
    int on;
    float level[MAX_PIXELS * 3];  /* phosphor glow per output byte */
};

static const monitor_model_t MODELS[MONITOR_MODEL_COUNT] = {
    {"Monochrome VGA", MONITOR_CONN_VGA, 0, 31000, 32000, 30.0f},
    {"Color VGA", MONITOR_CONN_VGA, 1, 31000, 32000, 2.0f},
    {"CGA color", MONITOR_CONN_DIGITAL, 1, 15500, 16000, 2.0f},
    {"Multisync", MONITOR_CONN_VGA, 1, 15000, 38000, 2.0f},
};

const monitor_model_t *monitor_builtin_model(int id)
{
    return id >= 0 && id < MONITOR_MODEL_COUNT ? &MODELS[id] : NULL;
}

monitor_t *monitor_create(const monitor_model_t *model)
{
    monitor_t *m = (monitor_t *)calloc(1, sizeof(monitor_t));
    if (!m) return NULL;
    m->model = model ? *model : MODELS[MONITOR_MODEL_VGA_MONO];
    m->on = 1;
    return m;
}

void monitor_destroy(monitor_t *m) { free(m); }
const monitor_model_t *monitor_model(const monitor_t *m) { return &m->model; }
void monitor_set_power(monitor_t *m, int on) { m->on = on != 0; }
int monitor_power(const monitor_t *m) { return m->on; }
int monitor_sense(const monitor_t *m) { return m->model.color ? VGA_SENSE_COLOR : VGA_SENSE_MONO; }
int monitor_channels(const monitor_t *m) { return m->model.color ? 3 : 1; }

int monitor_accepts(const monitor_t *m, const vga_signal_t *sig)
{
    return sig && sig->hfreq_hz >= m->model.hfreq_min_hz && sig->hfreq_hz <= m->model.hfreq_max_hz &&
           (size_t)sig->width * sig->height <= MAX_PIXELS;
}

int monitor_display(monitor_t *m, const vga_signal_t *sig, const uint8_t *rgb, float dt_seconds, uint8_t *out)
{
    size_t ch = (size_t)monitor_channels(m);
    int status;
    size_t pixels = sig ? (size_t)sig->width * sig->height : MAX_PIXELS, i;
    float decay = 0.0f;
    if (pixels > MAX_PIXELS) pixels = MAX_PIXELS;
    if (m->model.persistence_ms > 0.0f)
        decay = (float)pow(0.5, (double)dt_seconds * 1000.0 / (double)m->model.persistence_ms);

    if (!m->on) status = MONITOR_OFF;
    else if (!sig || !rgb) status = MONITOR_NO_SIGNAL;
    else if (!monitor_accepts(m, sig)) status = MONITOR_OUT_OF_RANGE;
    else status = MONITOR_SHOWING;

    for (i = 0; i < pixels; i++) {
        size_t c;
        for (c = 0; c < ch; c++) {
            float in = 0.0f, lv;
            if (status == MONITOR_SHOWING) {
                const uint8_t *p = rgb + 3 * i;
                in = ch == 1 ? (30.0f * p[0] + 59.0f * p[1] + 11.0f * p[2]) / 100.0f : (float)p[c];
            }
            lv = m->level[i * ch + c] * decay;
            if (in > lv) lv = in;
            m->level[i * ch + c] = lv;
            out[i * ch + c] = (uint8_t)(lv + 0.5f);
        }
    }
    return status;
}
