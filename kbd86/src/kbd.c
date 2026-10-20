/*
 * kbd86 - PC keyboard device and keyboard BIOS. See include/kbd.h.
 */
#include "kbd.h"

#include <stdlib.h>
#include <string.h>

#define FIFO_SIZE 32

/* BIOS data area (linear addresses) */
#define BDA_FLAGS1   0x417   /* shift flags: 0 RShift 1 LShift 2 Ctrl 3 Alt 4 Scroll 5 Num 6 Caps 7 Insert */
#define BDA_FLAGS2   0x418   /* 0 LCtrl 1 LAlt 2 SysRq 4 Scroll 5 Num 6 Caps 7 Insert (keys held) */
#define BDA_HEAD     0x41A
#define BDA_TAIL     0x41C
#define BDA_BUF      0x41E
#define BDA_START    0x480
#define BDA_END      0x482
#define BDA_FLAGS3   0x496   /* 1 last code was E0, 2 RCtrl, 3 RAlt, 4 enhanced keyboard */
#define BDA_LEDS     0x497

typedef struct kbd_state {
    uint8_t fifo[FIFO_SIZE];
    uint8_t fifo_head, fifo_count;
    uint8_t out, obf, irq_pending;
    uint8_t last_was_cmd;
    uint8_t cmd_byte;            /* 8042 command byte */
    uint8_t ctl_expect;          /* 1 = next port 60h write is the command byte */
    uint8_t kbd_expect;          /* 0, 0xED or 0xF3: parameter of a keyboard command */
    uint8_t scanning;            /* keyboard enabled (F4) / disabled (F5) */
    uint8_t leds;
    uint8_t typematic;           /* F3 parameter: bits 5-6 delay, 0-4 rate */
    uint16_t held_key;
    uint64_t time_us, next_repeat_us;
    uint8_t down[64];            /* 512 bits: normal and E0 keys currently pressed */
    uint8_t bios_e0, bios_skip;
} kbd_state_t;

struct kbd {
    kbd_state_t st;
    uint8_t *mem;
};

/* ---------- memory helpers ---------- */

static uint8_t rd8(const kbd_t *k, uint32_t a) { return k->mem[a]; }
static void wr8(kbd_t *k, uint32_t a, uint8_t v) { k->mem[a] = v; }
static uint16_t rd16(const kbd_t *k, uint32_t a) { return (uint16_t)(k->mem[a] | (k->mem[a + 1] << 8)); }
static void wr16(kbd_t *k, uint32_t a, uint16_t v) { k->mem[a] = (uint8_t)v; k->mem[a + 1] = (uint8_t)(v >> 8); }
static void setbit(kbd_t *k, uint32_t a, uint8_t bit, int on)
{
    if (on) k->mem[a] |= bit; else k->mem[a] &= (uint8_t)~bit;
}

/* ---------- keyboard -> controller byte stream ---------- */

static void fifo_push(kbd_t *k, uint8_t b)
{
    kbd_state_t *s = &k->st;
    if (s->fifo_count >= FIFO_SIZE) return;     /* overrun: the byte is lost */
    s->fifo[(s->fifo_head + s->fifo_count) % FIFO_SIZE] = b;
    s->fifo_count++;
}

static void fifo_clear(kbd_t *k) { k->st.fifo_count = 0; }

/* Moves the next byte into the output buffer when it is free. */
static void pump(kbd_t *k)
{
    kbd_state_t *s = &k->st;
    if (s->obf || !s->fifo_count || (s->cmd_byte & 0x10)) return;
    s->out = s->fifo[s->fifo_head];
    s->fifo_head = (uint8_t)((s->fifo_head + 1) % FIFO_SIZE);
    s->fifo_count--;
    s->obf = 1;
    if (s->cmd_byte & 0x01) s->irq_pending = 1;
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
    if ((key & 0x7F) == 0 || ((key >> 8) != 0 && (key >> 8) != 0xE0) || (key & 0x80)) return;
    idx = key_index(key);
    was = (s->down[idx >> 3] >> (idx & 7)) & 1;
    if (pressed == was) return;              /* host auto-repeat: the device repeats by itself */
    if (pressed) s->down[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    else s->down[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
    if (!s->scanning) return;
    send_key_bytes(k, key, !pressed);
    if (pressed) {
        s->held_key = key;
        s->next_repeat_us = s->time_us + typematic_delay_us(s->typematic);
    } else if (s->held_key == key) {
        s->held_key = 0;
    }
    pump(k);
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
    while (s->held_key && s->scanning && s->time_us >= s->next_repeat_us && guard++ < 64) {
        if (s->fifo_count <= FIFO_SIZE - 2) send_key_bytes(k, s->held_key, 0);
        s->next_repeat_us += typematic_period_us(s->typematic);
    }
    if (guard >= 64) s->next_repeat_us = s->time_us + typematic_period_us(s->typematic);
    pump(k);
}

int kbd_take_irq(kbd_t *k)
{
    if (!k->st.irq_pending) return 0;
    k->st.irq_pending = 0;
    return 1;
}

uint8_t kbd_leds(const kbd_t *k) { return k->st.leds; }

/* ---------- ports ---------- */

static void respond(kbd_t *k, uint8_t b) { fifo_push(k, b); }

static int keyboard_command(kbd_t *k, uint8_t v)
{
    kbd_state_t *s = &k->st;
    if (s->kbd_expect == 0xED) { s->leds = (uint8_t)(v & 7); s->kbd_expect = 0; respond(k, 0xFA); return KBD_PORT_HANDLED; }
    if (s->kbd_expect == 0xF3) { s->typematic = (uint8_t)(v & 0x7F); s->kbd_expect = 0; respond(k, 0xFA); return KBD_PORT_HANDLED; }
    switch (v) {
    case 0xED: case 0xF3:
        fifo_clear(k);
        s->kbd_expect = v;
        respond(k, 0xFA);
        return KBD_PORT_HANDLED;
    case 0xEE: respond(k, 0xEE); return KBD_PORT_HANDLED;                         /* echo */
    case 0xF2: fifo_clear(k); respond(k, 0xFA); respond(k, 0xAB); respond(k, 0x83); return KBD_PORT_HANDLED;  /* identify */
    case 0xF4: fifo_clear(k); s->scanning = 1; respond(k, 0xFA); return KBD_PORT_HANDLED;
    case 0xF5: fifo_clear(k); s->scanning = 0; s->typematic = 0x2B; s->held_key = 0; respond(k, 0xFA); return KBD_PORT_HANDLED;
    case 0xF6: fifo_clear(k); s->typematic = 0x2B; respond(k, 0xFA); return KBD_PORT_HANDLED;
    case 0xFF:                                                                     /* reset */
        fifo_clear(k);
        s->scanning = 1; s->typematic = 0x2B; s->held_key = 0; s->leds = 0;
        respond(k, 0xFA); respond(k, 0xAA);
        return KBD_PORT_HANDLED;
    default:
        return KBD_PORT_UNSUPPORTED;
    }
}

static int controller_command(kbd_t *k, uint8_t v)
{
    kbd_state_t *s = &k->st;
    switch (v) {
    case 0x20: respond(k, s->cmd_byte); return KBD_PORT_HANDLED;   /* read command byte */
    case 0x60: s->ctl_expect = 1; return KBD_PORT_HANDLED;         /* write command byte */
    case 0xAA: fifo_clear(k); respond(k, 0x55); return KBD_PORT_HANDLED;  /* self test */
    case 0xAB: respond(k, 0x00); return KBD_PORT_HANDLED;          /* interface test */
    case 0xAD: s->cmd_byte |= 0x10; return KBD_PORT_HANDLED;       /* disable keyboard */
    case 0xAE: s->cmd_byte &= (uint8_t)~0x10; return KBD_PORT_HANDLED;  /* enable keyboard */
    default: return KBD_PORT_UNSUPPORTED;                          /* A20, reset, mouse port... */
    }
}

int kbd_port_in(kbd_t *k, uint16_t port, uint8_t *value)
{
    kbd_state_t *s = &k->st;
    if (port == 0x60) {
        *value = s->out;
        s->obf = 0;
        pump(k);
        return KBD_PORT_HANDLED;
    }
    if (port == 0x64) {
        *value = (uint8_t)(s->obf | 0x04 | (s->last_was_cmd ? 0x08 : 0) | ((s->cmd_byte & 0x10) ? 0 : 0x10));
        return KBD_PORT_HANDLED;
    }
    return KBD_PORT_NOT_MINE;
}

int kbd_port_out(kbd_t *k, uint16_t port, uint8_t value)
{
    kbd_state_t *s = &k->st;
    int r;
    if (port == 0x60) {
        s->last_was_cmd = 0;
        if (s->ctl_expect) { s->cmd_byte = value; s->ctl_expect = 0; pump(k); return KBD_PORT_HANDLED; }
        r = keyboard_command(k, value);
        pump(k);
        return r;
    }
    if (port == 0x64) {
        s->last_was_cmd = 1;
        r = controller_command(k, value);
        pump(k);
        return r;
    }
    return KBD_PORT_NOT_MINE;
}

/* ---------- BIOS: scan code translation (US layout, as the BIOS without KEYB) ---------- */

/* For scan codes 01h-58h: normal, shift, ctrl, alt. 0 = no key. */
static const uint16_t XLAT[0x59][4] = {
    {0, 0, 0, 0},
    {0x011B, 0x011B, 0x011B, 0x0100}, /* 01 Esc */
    {0x0231, 0x0221, 0x0000, 0x7800}, {0x0332, 0x0340, 0x0300, 0x7900}, {0x0433, 0x0423, 0x0000, 0x7A00},
    {0x0534, 0x0524, 0x0000, 0x7B00}, {0x0635, 0x0625, 0x0000, 0x7C00}, {0x0736, 0x075E, 0x071E, 0x7D00},
    {0x0837, 0x0826, 0x0000, 0x7E00}, {0x0938, 0x092A, 0x0000, 0x7F00}, {0x0A39, 0x0A28, 0x0000, 0x8000},
    {0x0B30, 0x0B29, 0x0000, 0x8100}, {0x0C2D, 0x0C5F, 0x0C1F, 0x8200}, {0x0D3D, 0x0D2B, 0x0000, 0x8300},
    {0x0E08, 0x0E08, 0x0E7F, 0x0E00}, /* 0E Backspace */
    {0x0F09, 0x0F00, 0x9400, 0xA500}, /* 0F Tab */
    {0x1071, 0x1051, 0x1011, 0x1000}, {0x1177, 0x1157, 0x1117, 0x1100}, {0x1265, 0x1245, 0x1205, 0x1200},
    {0x1372, 0x1352, 0x1312, 0x1300}, {0x1474, 0x1454, 0x1414, 0x1400}, {0x1579, 0x1559, 0x1519, 0x1500},
    {0x1675, 0x1655, 0x1615, 0x1600}, {0x1769, 0x1749, 0x1709, 0x1700}, {0x186F, 0x184F, 0x180F, 0x1800},
    {0x1970, 0x1950, 0x1910, 0x1900},
    {0x1A5B, 0x1A7B, 0x1A1B, 0x1A00}, {0x1B5D, 0x1B7D, 0x1B1D, 0x1B00}, /* [ ] */
    {0x1C0D, 0x1C0D, 0x1C0A, 0x1C00}, /* 1C Enter */
    {0, 0, 0, 0},                     /* 1D Ctrl */
    {0x1E61, 0x1E41, 0x1E01, 0x1E00}, {0x1F73, 0x1F53, 0x1F13, 0x1F00}, {0x2064, 0x2044, 0x2004, 0x2000},
    {0x2166, 0x2146, 0x2106, 0x2100}, {0x2267, 0x2247, 0x2207, 0x2200}, {0x2368, 0x2348, 0x2308, 0x2300},
    {0x246A, 0x244A, 0x240A, 0x2400}, {0x256B, 0x254B, 0x250B, 0x2500}, {0x266C, 0x264C, 0x260C, 0x2600},
    {0x273B, 0x273A, 0x0000, 0x2700}, {0x2827, 0x2822, 0x0000, 0x2800}, {0x2960, 0x297E, 0x0000, 0x2900},
    {0, 0, 0, 0},                     /* 2A LShift */
    {0x2B5C, 0x2B7C, 0x2B1C, 0x2B00}, /* \ */
    {0x2C7A, 0x2C5A, 0x2C1A, 0x2C00}, {0x2D78, 0x2D58, 0x2D18, 0x2D00}, {0x2E63, 0x2E43, 0x2E03, 0x2E00},
    {0x2F76, 0x2F56, 0x2F16, 0x2F00}, {0x3062, 0x3042, 0x3002, 0x3000}, {0x316E, 0x314E, 0x310E, 0x3100},
    {0x326D, 0x324D, 0x320D, 0x3200},
    {0x332C, 0x333C, 0x0000, 0x3300}, {0x342E, 0x343E, 0x0000, 0x3400}, {0x352F, 0x353F, 0x0000, 0x3500},
    {0, 0, 0, 0},                     /* 36 RShift */
    {0x372A, 0x372A, 0x9600, 0x3700}, /* 37 keypad * */
    {0, 0, 0, 0},                     /* 38 Alt */
    {0x3920, 0x3920, 0x3920, 0x3920}, /* 39 Space */
    {0, 0, 0, 0},                     /* 3A Caps Lock */
    /* 3B-44 F1-F10 are generated in translate() */
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0},       /* 45 Num Lock, 46 Scroll Lock */
    /* 47-53 keypad: normal = Num Lock off, shift = digits */
    {0x4700, 0x4737, 0x7700, 0}, {0x4800, 0x4838, 0x8D00, 0}, {0x4900, 0x4939, 0x8400, 0},
    {0x4A2D, 0x4A2D, 0x8E00, 0x4A00},
    {0x4B00, 0x4B34, 0x7300, 0}, {0x4C00, 0x4C35, 0x8F00, 0}, {0x4D00, 0x4D36, 0x7400, 0},
    {0x4E2B, 0x4E2B, 0x9000, 0x4E00},
    {0x4F00, 0x4F31, 0x7500, 0}, {0x5000, 0x5032, 0x9100, 0}, {0x5100, 0x5133, 0x7600, 0},
    {0x5200, 0x5230, 0x9200, 0}, {0x5300, 0x532E, 0x9300, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0},       /* 54 SysRq, 55 */
    {0x565C, 0x567C, 0x0000, 0x0000}, /* 56 extra key of ISO keyboards */
    {0x8500, 0x8700, 0x8900, 0x8B00}, /* 57 F11 */
    {0x8600, 0x8800, 0x8A00, 0x8C00}, /* 58 F12 */
};

/* Gray navigation keys (E0 prefix): ctrl and alt codes, by scan code 47h-53h */
static uint16_t gray_ctrl(uint8_t sc)
{
    switch (sc) {
    case 0x47: return 0x77E0; case 0x48: return 0x8DE0; case 0x49: return 0x84E0;
    case 0x4B: return 0x73E0; case 0x4D: return 0x74E0; case 0x4F: return 0x75E0;
    case 0x50: return 0x91E0; case 0x51: return 0x76E0; case 0x52: return 0x92E0; case 0x53: return 0x93E0;
    default: return 0;
    }
}

static uint16_t gray_alt(uint8_t sc)
{
    switch (sc) {
    case 0x47: return 0x9700; case 0x48: return 0x9800; case 0x49: return 0x9900;
    case 0x4B: return 0x9B00; case 0x4D: return 0x9D00; case 0x4F: return 0x9F00;
    case 0x50: return 0xA000; case 0x51: return 0xA100; case 0x52: return 0xA200; case 0x53: return 0xA300;
    default: return 0;
    }
}

static int is_letter(uint8_t sc)
{
    return (sc >= 0x10 && sc <= 0x19) || (sc >= 0x1E && sc <= 0x26) || (sc >= 0x2C && sc <= 0x32);
}

static uint16_t translate(kbd_t *k, uint8_t sc, int ext)
{
    uint8_t f1 = rd8(k, BDA_FLAGS1);
    int shift = (f1 & 0x03) != 0, ctrl = (f1 & 0x04) != 0, alt = (f1 & 0x08) != 0;
    int caps = (f1 & 0x40) != 0, num = (f1 & 0x20) != 0;

    if (ext) {
        if (sc == 0x1C) return alt ? 0xA600 : ctrl ? 0xE00A : 0xE00D;     /* keypad Enter */
        if (sc == 0x35) return alt ? 0xA400 : ctrl ? 0x9500 : 0xE02F;     /* keypad / */
        if (sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4C && sc != 0x4E) {
            if (alt) return gray_alt(sc);
            if (ctrl) return gray_ctrl(sc);
            return (uint16_t)((sc << 8) | 0xE0);
        }
        return 0;                                    /* Windows keys and others: no code */
    }
    if (sc >= 0x3B && sc <= 0x44) {                  /* F1-F10 */
        if (alt) return (uint16_t)((sc + 0x2D) << 8);
        if (ctrl) return (uint16_t)((sc + 0x23) << 8);
        if (shift) return (uint16_t)((sc + 0x19) << 8);
        return (uint16_t)(sc << 8);
    }
    if (sc > 0x58) return 0;
    if (alt) return XLAT[sc][3];
    if (ctrl) return XLAT[sc][2];
    if (sc >= 0x47 && sc <= 0x53) return XLAT[sc][(num ^ shift) ? 1 : 0];
    if (is_letter(sc)) return XLAT[sc][(caps ^ shift) ? 1 : 0];
    return XLAT[sc][shift ? 1 : 0];
}

static int buffer_put(kbd_t *k, uint16_t code)
{
    uint16_t start = rd16(k, BDA_START), end = rd16(k, BDA_END);
    uint16_t tail = rd16(k, BDA_TAIL), head = rd16(k, BDA_HEAD);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= end) next = start;
    if (next == head) return 0;                      /* full: the BIOS would beep */
    wr16(k, 0x400u + tail, code);
    wr16(k, BDA_TAIL, next);
    return 1;
}

static void update_leds(kbd_t *k)
{
    uint8_t f1 = rd8(k, BDA_FLAGS1);
    k->st.leds = (uint8_t)(((f1 >> 4) & 1) | (((f1 >> 5) & 1) << 1) | (((f1 >> 6) & 1) << 2));
    wr8(k, BDA_LEDS, (uint8_t)((rd8(k, BDA_LEDS) & 0xF8) | k->st.leds));
}

static void toggle_lock(kbd_t *k, uint8_t held_bit, uint8_t lock_bit, int brk)
{
    if (brk) { setbit(k, BDA_FLAGS2, held_bit, 0); return; }
    if (rd8(k, BDA_FLAGS2) & held_bit) return;       /* still held: no toggle on repeats */
    setbit(k, BDA_FLAGS2, held_bit, 1);
    wr8(k, BDA_FLAGS1, (uint8_t)(rd8(k, BDA_FLAGS1) ^ lock_bit));
    update_leds(k);
}

void kbd_bios_irq(kbd_t *k)
{
    kbd_state_t *s = &k->st;
    uint8_t b, sc;
    int brk, ext;

    kbd_port_in(k, 0x60, &b);
    if (s->bios_skip) { s->bios_skip--; return; }
    if (b == 0xE1) { s->bios_skip = 5; return; }     /* Pause: E1 1D 45 E1 9D C5, ignored */
    if (b == 0xE0) { s->bios_e0 = 1; setbit(k, BDA_FLAGS3, 0x02, 1); return; }
    /* keyboard responses; AAh is not here: it is also the break code of left
     * Shift, and the real BIOS treats it as such (harmless after a reset) */
    if (b == 0xFA || b == 0xFE || b == 0xEE || b == 0x00 || b == 0xFF) return;

    sc = (uint8_t)(b & 0x7F);
    brk = (b & 0x80) != 0;
    ext = s->bios_e0;
    s->bios_e0 = 0;
    setbit(k, BDA_FLAGS3, 0x02, 0);

    switch (sc) {
    case 0x2A: if (!ext) setbit(k, BDA_FLAGS1, 0x02, !brk); return;      /* left Shift (E0 2A = fake shift) */
    case 0x36: if (!ext) setbit(k, BDA_FLAGS1, 0x01, !brk); return;      /* right Shift */
    case 0x1D:
        if (ext) setbit(k, BDA_FLAGS3, 0x04, !brk); else setbit(k, BDA_FLAGS2, 0x01, !brk);
        setbit(k, BDA_FLAGS1, 0x04, (rd8(k, BDA_FLAGS3) & 0x04) || (rd8(k, BDA_FLAGS2) & 0x01));
        return;
    case 0x38:
        if (ext) setbit(k, BDA_FLAGS3, 0x08, !brk); else setbit(k, BDA_FLAGS2, 0x02, !brk);
        setbit(k, BDA_FLAGS1, 0x08, (rd8(k, BDA_FLAGS3) & 0x08) || (rd8(k, BDA_FLAGS2) & 0x02));
        return;
    case 0x3A: if (!ext) toggle_lock(k, 0x40, 0x40, brk); return;         /* Caps Lock */
    case 0x45: if (!ext) toggle_lock(k, 0x20, 0x20, brk); return;         /* Num Lock */
    case 0x46: if (!ext) toggle_lock(k, 0x10, 0x10, brk); return;         /* Scroll Lock */
    default: break;
    }
    if (brk) {
        if (sc == 0x52) setbit(k, BDA_FLAGS2, 0x80, 0);
        return;
    }
    if (sc == 0x52) {
        /* Insert toggles insert mode unless it types a digit (keypad with Num Lock) */
        uint8_t f1 = rd8(k, BDA_FLAGS1);
        int digit = !ext && !(f1 & 0x0C) && (((f1 & 0x20) != 0) ^ ((f1 & 0x03) != 0));
        if (!digit && !(f1 & 0x0C) && !(rd8(k, BDA_FLAGS2) & 0x80)) {
            wr8(k, BDA_FLAGS1, (uint8_t)(f1 ^ 0x80));
            setbit(k, BDA_FLAGS2, 0x80, 1);
        }
    }
    {
        uint16_t code = translate(k, sc, ext);
        if (code) buffer_put(k, code);
    }
}

/* ---------- BIOS: INT 16h ---------- */

/* Converts an enhanced keystroke for the standard functions (00h, 01h).
 * Returns 0 if the standard functions discard it. */
static uint16_t to_standard(uint16_t code)
{
    uint8_t scan = (uint8_t)(code >> 8), ascii = (uint8_t)code;
    if (scan == 0xE0) return (uint16_t)(((ascii == 0x2F) ? 0x3500 : 0x1C00) | ascii);  /* keypad Enter, / */
    if (scan > 0x84) return 0;                       /* keys of the 101-key keyboard only */
    if (ascii == 0xE0 && scan) return (uint16_t)(scan << 8);
    return code;
}

static int buffer_peek(kbd_t *k, uint16_t *code)
{
    uint16_t head = rd16(k, BDA_HEAD);
    if (head == rd16(k, BDA_TAIL)) return 0;
    *code = rd16(k, 0x400u + head);
    return 1;
}

static void buffer_drop(kbd_t *k)
{
    uint16_t head = (uint16_t)(rd16(k, BDA_HEAD) + 2);
    if (head >= rd16(k, BDA_END)) head = rd16(k, BDA_START);
    wr16(k, BDA_HEAD, head);
}

/* A program can corrupt the buffer pointers in the BIOS data area; the loops
 * below are bounded so the host can never get stuck in them. */
#define MAX_SCAN 64

int kbd_int16(kbd_t *k, kbd_bios_regs_t *r)
{
    uint8_t ah = (uint8_t)(r->ax >> 8), al = (uint8_t)r->ax;
    uint16_t code;
    int guard;
    switch (ah) {
    case 0x00: case 0x10:
        for (guard = 0; guard < MAX_SCAN; guard++) {
            if (!buffer_peek(k, &code)) return KBD_BIOS_WAIT;
            buffer_drop(k);
            if (ah == 0x00) code = to_standard(code);
            if (code) { r->ax = code; return KBD_BIOS_DONE; }
        }
        return KBD_BIOS_WAIT;
    case 0x01: case 0x11:
        for (guard = 0; guard < MAX_SCAN; guard++) {
            if (!buffer_peek(k, &code)) { r->flags |= 0x40; return KBD_BIOS_DONE; }
            if (ah == 0x01) {
                uint16_t c = to_standard(code);
                if (!c) { buffer_drop(k); continue; }
                code = c;
            }
            r->ax = code;
            r->flags &= (uint16_t)~0x40;
            return KBD_BIOS_DONE;
        }
        r->flags |= 0x40;
        return KBD_BIOS_DONE;
    case 0x02:
        r->ax = (uint16_t)((r->ax & 0xFF00) | rd8(k, BDA_FLAGS1));
        return KBD_BIOS_DONE;
    case 0x12: {
        uint8_t f2 = rd8(k, BDA_FLAGS2), f3 = rd8(k, BDA_FLAGS3);
        uint8_t hi = (uint8_t)((f2 & 0x01) | (f2 & 0x02) | (f3 & 0x04) | (f3 & 0x08) |
                               (f2 & 0x10) | (f2 & 0x20) | (f2 & 0x40) | ((f2 & 0x04) << 5));
        r->ax = (uint16_t)((hi << 8) | rd8(k, BDA_FLAGS1));
        return KBD_BIOS_DONE;
    }
    case 0x03:
        if (al == 0x05) {
            uint8_t delay = (uint8_t)(r->bx >> 8), rate = (uint8_t)r->bx;
            if (delay > 3 || rate > 0x1F) return KBD_BIOS_UNSUPPORTED;
            k->st.typematic = (uint8_t)((delay << 5) | rate);
            return KBD_BIOS_DONE;
        }
        if (al == 0x00) { k->st.typematic = 0x2B; return KBD_BIOS_DONE; }
        return KBD_BIOS_UNSUPPORTED;
    case 0x05:
        r->ax = (uint16_t)((r->ax & 0xFF00) | (buffer_put(k, r->cx) ? 0 : 1));
        return KBD_BIOS_DONE;
    default:
        return KBD_BIOS_UNSUPPORTED;
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

kbd_t *kbd_create(uint8_t *memory)
{
    kbd_t *k;
    if (!memory) return NULL;
    k = (kbd_t *)calloc(1, sizeof(kbd_t));
    if (!k) return NULL;
    k->mem = memory;
    kbd_reset(k);
    return k;
}

void kbd_destroy(kbd_t *k) { free(k); }

void kbd_reset(kbd_t *k)
{
    memset(&k->st, 0, sizeof(k->st));
    k->st.cmd_byte = 0x45;        /* IRQ 1 enabled, system flag, scan code translation */
    k->st.scanning = 1;
    k->st.typematic = 0x2B;       /* 500 ms delay, 10.9 characters per second */
    wr8(k, BDA_FLAGS1, 0x20);     /* Num Lock on at boot */
    wr8(k, BDA_FLAGS2, 0);
    wr16(k, BDA_HEAD, 0x1E);
    wr16(k, BDA_TAIL, 0x1E);
    wr16(k, BDA_START, 0x1E);
    wr16(k, BDA_END, 0x3E);
    wr8(k, BDA_FLAGS3, 0x10);     /* enhanced (101-key) keyboard */
    wr8(k, BDA_LEDS, 0);
    update_leds(k);
}

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
