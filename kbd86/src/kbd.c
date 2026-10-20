/*
 * kbd86 - PC keyboard (the peripheral). See include/kbd.h.
 */
#include "kbd.h"

#include <stdlib.h>
#include <string.h>

#define FIFO_SIZE 32

typedef struct kbd_state {
    uint8_t fifo[FIFO_SIZE];     /* bytes waiting to go down the wire */
    uint8_t fifo_head, fifo_count;
    uint8_t powered;
    uint8_t expect;              /* 0, 0xED or 0xF3: parameter of a command */
    uint8_t scanning;            /* enabled (F4) / disabled (F5) */
    uint8_t leds;
    uint8_t typematic;           /* F3 parameter: bits 5-6 delay, 0-4 rate */
    uint16_t held_key;
    uint64_t time_us, next_repeat_us;
    uint8_t down[64];            /* 512 bits: normal and E0 keys physically pressed */
} kbd_state_t;

struct kbd {
    kbd_state_t st;
    kbd_model_t model;
};

static const kbd_model_t MODELS[KBD_MODEL_COUNT] = {
    {"IBM AT 84-key", KBD_KEYS_AT84, 0, {0, 0}, 1},
    {"Enhanced 101-key", KBD_KEYS_ENHANCED, 2, {0xAB, 0x83}, 1},
    {"Enhanced 102-key", KBD_KEYS_ENHANCED | KBD_KEYS_ISO, 2, {0xAB, 0x83}, 1},
    {"Windows 104-key", KBD_KEYS_ENHANCED | KBD_KEYS_WINDOWS, 2, {0xAB, 0x83}, 1},
};

const kbd_model_t *kbd_builtin_model(int id)
{
    return id >= 0 && id < KBD_MODEL_COUNT ? &MODELS[id] : NULL;
}

int kbd_has_key(const kbd_model_t *m, uint16_t key)
{
    uint8_t sc = (uint8_t)(key & 0x7F);
    if (sc == 0 || (key & 0x80) || ((key >> 8) != 0 && (key >> 8) != 0xE0)) return 0;
    if ((key >> 8) == 0xE0) {
        switch (sc) {
        case 0x1C: case 0x1D: case 0x35: case 0x38: case 0x47: case 0x48: case 0x49:
        case 0x4B: case 0x4D: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53:
            return (m->keys & KBD_KEYS_ENHANCED) != 0;
        case 0x5B: case 0x5C: case 0x5D:
            return (m->keys & KBD_KEYS_WINDOWS) != 0;
        default:
            return 0;
        }
    }
    if (sc <= 0x53) return (m->keys & (KBD_KEYS_AT84 | KBD_KEYS_ENHANCED)) != 0;
    if (sc == 0x54) return (m->keys & KBD_KEYS_AT84) != 0;          /* SysRq of the AT keyboard */
    if (sc == 0x56) return (m->keys & KBD_KEYS_ISO) != 0;
    if (sc == 0x57 || sc == 0x58) return (m->keys & KBD_KEYS_ENHANCED) != 0;
    return 0;
}

/* ---------- byte stream to the controller ---------- */

static void fifo_push(kbd_t *k, uint8_t b)
{
    kbd_state_t *s = &k->st;
    if (s->fifo_count >= FIFO_SIZE) return;     /* overrun: the byte is lost */
    s->fifo[(s->fifo_head + s->fifo_count) % FIFO_SIZE] = b;
    s->fifo_count++;
}

static void fifo_clear(kbd_t *k) { k->st.fifo_count = 0; }

int kbd_wire_read(kbd_t *k, uint8_t *byte)
{
    kbd_state_t *s = &k->st;
    if (!s->powered || !s->fifo_count) return 0;
    *byte = s->fifo[s->fifo_head];
    s->fifo_head = (uint8_t)((s->fifo_head + 1) % FIFO_SIZE);
    s->fifo_count--;
    return 1;
}

static void send_key_bytes(kbd_t *k, uint16_t key, int released)
{
    if ((key >> 8) == 0xE0) fifo_push(k, 0xE0);
    fifo_push(k, (uint8_t)((key & 0x7F) | (released ? 0x80 : 0)));
}

static int key_index(uint16_t key) { return ((key >> 8) == 0xE0 ? 256 : 0) + (key & 0x7F); }

static uint32_t typematic_delay_us(uint8_t t) { return 250000u * (((t >> 5) & 3) + 1); }

static uint32_t typematic_period_us(uint8_t t)
{
    uint32_t a = t & 7, b = (t >> 3) & 3;
    return (8 + a) * (1u << b) * 4170u;      /* 30 to 2 characters per second */
}

void kbd_key(kbd_t *k, uint16_t key, int pressed)
{
    kbd_state_t *s = &k->st;
    int idx, was;
    if (!kbd_has_key(&k->model, key)) return;
    idx = key_index(key);
    was = (s->down[idx >> 3] >> (idx & 7)) & 1;
    if (pressed == was) return;              /* host auto-repeat: the keyboard repeats by itself */
    if (pressed) s->down[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    else s->down[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
    if (!s->powered || !s->scanning) return;
    send_key_bytes(k, key, !pressed);
    if (pressed) {
        s->held_key = key;
        s->next_repeat_us = s->time_us + typematic_delay_us(s->typematic);
    } else if (s->held_key == key) {
        s->held_key = 0;
    }
}

void kbd_release_all(kbd_t *k)
{
    int i;
    for (i = 0; i < 512; i++) {
        if (k->st.down[i >> 3] & (1u << (i & 7))) {
            uint16_t key = (uint16_t)((i >= 256 ? 0xE000 : 0) | (i & 0x7F));
            kbd_key(k, key, 0);
        }
    }
}

void kbd_advance(kbd_t *k, uint32_t microseconds)
{
    kbd_state_t *s = &k->st;
    int guard = 0;
    s->time_us += microseconds;
    if (!s->powered) return;
    while (s->held_key && s->scanning && s->time_us >= s->next_repeat_us && guard++ < 64) {
        if (s->fifo_count <= FIFO_SIZE - 2) send_key_bytes(k, s->held_key, 0);
        s->next_repeat_us += typematic_period_us(s->typematic);
    }
    if (guard >= 64) s->next_repeat_us = s->time_us + typematic_period_us(s->typematic);
}

uint8_t kbd_leds(const kbd_t *k) { return k->st.powered && k->model.has_leds ? k->st.leds : 0; }

/* ---------- commands from the controller ---------- */

static void respond(kbd_t *k, uint8_t b) { fifo_push(k, b); }

static void self_test(kbd_t *k)
{
    kbd_state_t *s = &k->st;
    fifo_clear(k);
    s->expect = 0;
    s->scanning = 1;
    s->typematic = 0x2B;          /* 500 ms delay, 10.9 characters per second */
    s->held_key = 0;
    s->leds = 0;
}

int kbd_wire_write(kbd_t *k, uint8_t v)
{
    kbd_state_t *s = &k->st;
    int i;
    if (!s->powered) return KBD_WIRE_HANDLED;          /* nobody listens: the controller times out */
    if (s->expect == 0xED) { s->leds = (uint8_t)(v & 7); s->expect = 0; respond(k, 0xFA); return KBD_WIRE_HANDLED; }
    if (s->expect == 0xF3) { s->typematic = (uint8_t)(v & 0x7F); s->expect = 0; respond(k, 0xFA); return KBD_WIRE_HANDLED; }
    switch (v) {
    case 0xED: case 0xF3:
        fifo_clear(k);
        s->expect = v;
        respond(k, 0xFA);
        return KBD_WIRE_HANDLED;
    case 0xEE: respond(k, 0xEE); return KBD_WIRE_HANDLED;                          /* echo */
    case 0xF2:                                                                      /* identify */
        fifo_clear(k);
        respond(k, 0xFA);
        for (i = 0; i < k->model.id_len && i < 2; i++) respond(k, k->model.id[i]);
        return KBD_WIRE_HANDLED;
    case 0xF4: fifo_clear(k); s->scanning = 1; respond(k, 0xFA); return KBD_WIRE_HANDLED;
    case 0xF5: fifo_clear(k); s->scanning = 0; s->typematic = 0x2B; s->held_key = 0; respond(k, 0xFA); return KBD_WIRE_HANDLED;
    case 0xF6: fifo_clear(k); s->typematic = 0x2B; respond(k, 0xFA); return KBD_WIRE_HANDLED;
    case 0xFF:                                                                      /* reset */
        self_test(k);
        respond(k, 0xFA); respond(k, 0xAA);
        return KBD_WIRE_HANDLED;
    default:
        return KBD_WIRE_UNSUPPORTED;               /* scan code sets (F0h), F7h-FDh, resend... */
    }
}

/* ---------- key names ---------- */

typedef struct { const char *code; uint16_t key; } keyname_t;

static const keyname_t KEYS[] = {
    {"Escape", 0x01}, {"Digit1", 0x02}, {"Digit2", 0x03}, {"Digit3", 0x04}, {"Digit4", 0x05},
    {"Digit5", 0x06}, {"Digit6", 0x07}, {"Digit7", 0x08}, {"Digit8", 0x09}, {"Digit9", 0x0A},
    {"Digit0", 0x0B}, {"Minus", 0x0C}, {"Equal", 0x0D}, {"Backspace", 0x0E}, {"Tab", 0x0F},
    {"KeyQ", 0x10}, {"KeyW", 0x11}, {"KeyE", 0x12}, {"KeyR", 0x13}, {"KeyT", 0x14}, {"KeyY", 0x15},
    {"KeyU", 0x16}, {"KeyI", 0x17}, {"KeyO", 0x18}, {"KeyP", 0x19}, {"BracketLeft", 0x1A},
    {"BracketRight", 0x1B}, {"Enter", 0x1C}, {"ControlLeft", 0x1D},
    {"KeyA", 0x1E}, {"KeyS", 0x1F}, {"KeyD", 0x20}, {"KeyF", 0x21}, {"KeyG", 0x22}, {"KeyH", 0x23},
    {"KeyJ", 0x24}, {"KeyK", 0x25}, {"KeyL", 0x26}, {"Semicolon", 0x27}, {"Quote", 0x28},
    {"Backquote", 0x29}, {"ShiftLeft", 0x2A}, {"Backslash", 0x2B},
    {"KeyZ", 0x2C}, {"KeyX", 0x2D}, {"KeyC", 0x2E}, {"KeyV", 0x2F}, {"KeyB", 0x30}, {"KeyN", 0x31},
    {"KeyM", 0x32}, {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35}, {"ShiftRight", 0x36},
    {"NumpadMultiply", 0x37}, {"AltLeft", 0x38}, {"Space", 0x39}, {"CapsLock", 0x3A},
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40},
    {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"NumLock", 0x45}, {"ScrollLock", 0x46},
    {"Numpad7", 0x47}, {"Numpad8", 0x48}, {"Numpad9", 0x49}, {"NumpadSubtract", 0x4A},
    {"Numpad4", 0x4B}, {"Numpad5", 0x4C}, {"Numpad6", 0x4D}, {"NumpadAdd", 0x4E},
    {"Numpad1", 0x4F}, {"Numpad2", 0x50}, {"Numpad3", 0x51}, {"Numpad0", 0x52}, {"NumpadDecimal", 0x53},
    {"IntlBackslash", 0x56}, {"F11", 0x57}, {"F12", 0x58},
    {"NumpadEnter", 0xE01C}, {"ControlRight", 0xE01D}, {"NumpadDivide", 0xE035}, {"AltRight", 0xE038},
    {"Home", 0xE047}, {"ArrowUp", 0xE048}, {"PageUp", 0xE049}, {"ArrowLeft", 0xE04B},
    {"ArrowRight", 0xE04D}, {"End", 0xE04F}, {"ArrowDown", 0xE050}, {"PageDown", 0xE051},
    {"Insert", 0xE052}, {"Delete", 0xE053}, {"MetaLeft", 0xE05B}, {"MetaRight", 0xE05C},
    {"ContextMenu", 0xE05D},
    {NULL, 0}
};

uint16_t kbd_key_from_code(const char *code)
{
    int i;
    if (!code) return 0;
    for (i = 0; KEYS[i].code; i++) if (!strcmp(KEYS[i].code, code)) return KEYS[i].key;
    return 0;
}

/* ---------- lifecycle ---------- */

kbd_t *kbd_create(const kbd_model_t *model)
{
    kbd_t *k = (kbd_t *)calloc(1, sizeof(kbd_t));
    if (!k) return NULL;
    k->model = model ? *model : MODELS[KBD_MODEL_ENHANCED101];
    self_test(k);
    return k;
}

void kbd_destroy(kbd_t *k) { free(k); }

const kbd_model_t *kbd_model(const kbd_t *k) { return &k->model; }

void kbd_power(kbd_t *k, int on)
{
    on = on != 0;
    if (on == k->st.powered) return;
    k->st.powered = (uint8_t)on;
    self_test(k);
    if (on) respond(k, 0xAA);                  /* basic assurance test passed */
}

int kbd_powered(const kbd_t *k) { return k->st.powered; }

size_t kbd_state_size(void) { return sizeof(kbd_state_t); }

size_t kbd_save_state(const kbd_t *k, void *buf, size_t len)
{
    if (len < sizeof(kbd_state_t)) return 0;
    memcpy(buf, &k->st, sizeof(kbd_state_t));
    return sizeof(kbd_state_t);
}

int kbd_load_state(kbd_t *k, const void *buf, size_t len)
{
    if (len != sizeof(kbd_state_t)) return -1;
    memcpy(&k->st, buf, sizeof(kbd_state_t));
    return 0;
}
