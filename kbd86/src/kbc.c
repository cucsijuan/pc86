/*
 * kbd86 - keyboard controller (8042) and keyboard BIOS. See include/kbc.h.
 */
#include "kbc.h"

#include <stdlib.h>
#include <string.h>

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

#define STATUS_TIMEOUT 0x40

typedef struct kbc_state {
    uint8_t out, obf, irq_pending;
    uint8_t last_was_cmd;
    uint8_t cmd_byte;            /* 8042 command byte */
    uint8_t ctl_expect;          /* 1 = next port 60h write is the command byte */
    uint8_t timeout;             /* last byte sent to the keyboard got no answer */
    uint8_t bios_e0, bios_skip;
} kbc_state_t;

struct kbc {
    kbc_state_t st;
    uint8_t *mem;
    kbc_device_t dev;
    int has_dev;
};

/* ---------- memory helpers ---------- */

static uint8_t rd8(const kbc_t *c, uint32_t a) { return c->mem[a]; }
static void wr8(kbc_t *c, uint32_t a, uint8_t v) { c->mem[a] = v; }
static uint16_t rd16(const kbc_t *c, uint32_t a) { return (uint16_t)(c->mem[a] | (c->mem[a + 1] << 8)); }
static void wr16(kbc_t *c, uint32_t a, uint16_t v) { c->mem[a] = (uint8_t)v; c->mem[a + 1] = (uint8_t)(v >> 8); }
static void setbit(kbc_t *c, uint32_t a, uint8_t bit, int on)
{
    if (on) c->mem[a] |= bit; else c->mem[a] &= (uint8_t)~bit;
}

/* ---------- the wire to the keyboard ---------- */

static int dev_read(void *user, uint8_t *b) { return kbd_wire_read((kbd_t *)user, b); }
static int dev_write(void *user, uint8_t b) { return kbd_wire_write((kbd_t *)user, b); }

kbc_device_t kbc_keyboard_device(kbd_t *keyboard)
{
    kbc_device_t d;
    d.user = keyboard;
    d.read = dev_read;
    d.write = dev_write;
    return d;
}

/* Moves the next byte from the keyboard into the output buffer when it is free. */
static void pump(kbc_t *c)
{
    kbc_state_t *s = &c->st;
    uint8_t b;
    if (s->obf || (s->cmd_byte & 0x10) || !c->has_dev || !c->dev.read) return;
    if (!c->dev.read(c->dev.user, &b)) return;
    s->out = b;
    s->obf = 1;
    s->timeout = 0;
    if (s->cmd_byte & 0x01) s->irq_pending = 1;
}

/* Places a byte in the output buffer as if it came from the controller. */
static void respond(kbc_t *c, uint8_t b)
{
    kbc_state_t *s = &c->st;
    s->out = b;
    s->obf = 1;
    if (s->cmd_byte & 0x01) s->irq_pending = 1;
}

static int send_to_device(kbc_t *c, uint8_t b)
{
    if (!c->has_dev || !c->dev.write) {
        c->st.timeout = 1;
        respond(c, 0xFE);
        return KBD_WIRE_HANDLED;
    }
    return c->dev.write(c->dev.user, b);
}

int kbc_take_irq(kbc_t *c)
{
    pump(c);
    if (!c->st.irq_pending) return 0;
    c->st.irq_pending = 0;
    return 1;
}

void kbc_connect(kbc_t *c, const kbc_device_t *device)
{
    if (device) { c->dev = *device; c->has_dev = 1; }
    else { memset(&c->dev, 0, sizeof(c->dev)); c->has_dev = 0; }
}

int kbc_connected(const kbc_t *c) { return c->has_dev; }

/* ---------- ports ---------- */

static int controller_command(kbc_t *c, uint8_t v)
{
    kbc_state_t *s = &c->st;
    switch (v) {
    case 0x20: respond(c, s->cmd_byte); return KBC_PORT_HANDLED;   /* read command byte */
    case 0x60: s->ctl_expect = 1; return KBC_PORT_HANDLED;         /* write command byte */
    case 0xAA: respond(c, 0x55); return KBC_PORT_HANDLED;          /* self test */
    case 0xAB: respond(c, c->has_dev ? 0x00 : 0x02); return KBC_PORT_HANDLED;  /* interface test: 02 = clock stuck low */
    case 0xAD: s->cmd_byte |= 0x10; return KBC_PORT_HANDLED;       /* disable keyboard */
    case 0xAE: s->cmd_byte &= (uint8_t)~0x10; return KBC_PORT_HANDLED;  /* enable keyboard */
    default: return KBC_PORT_UNSUPPORTED;                          /* A20, reset, mouse port... */
    }
}

int kbc_port_in(kbc_t *c, uint16_t port, uint8_t *value)
{
    kbc_state_t *s = &c->st;
    if (port == 0x60) {
        *value = s->out;
        s->obf = 0;
        pump(c);
        return KBC_PORT_HANDLED;
    }
    if (port == 0x64) {
        pump(c);
        *value = (uint8_t)(s->obf | 0x04 | (s->last_was_cmd ? 0x08 : 0) | ((s->cmd_byte & 0x10) ? 0 : 0x10) |
                           (s->timeout ? STATUS_TIMEOUT : 0));
        return KBC_PORT_HANDLED;
    }
    return KBC_PORT_NOT_MINE;
}

int kbc_port_out(kbc_t *c, uint16_t port, uint8_t value)
{
    kbc_state_t *s = &c->st;
    int r;
    if (port == 0x60) {
        s->last_was_cmd = 0;
        if (s->ctl_expect) { s->cmd_byte = value; s->ctl_expect = 0; pump(c); return KBC_PORT_HANDLED; }
        s->timeout = 0;
        r = send_to_device(c, value) == KBD_WIRE_UNSUPPORTED ? KBC_PORT_UNSUPPORTED : KBC_PORT_HANDLED;
        pump(c);
        return r;
    }
    if (port == 0x64) {
        s->last_was_cmd = 1;
        r = controller_command(c, value);
        pump(c);
        return r;
    }
    return KBC_PORT_NOT_MINE;
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

static uint16_t translate(kbc_t *c, uint8_t sc, int ext)
{
    uint8_t f1 = rd8(c, BDA_FLAGS1);
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

static int buffer_put(kbc_t *c, uint16_t code)
{
    uint16_t start = rd16(c, BDA_START), end = rd16(c, BDA_END);
    uint16_t tail = rd16(c, BDA_TAIL), head = rd16(c, BDA_HEAD);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= end) next = start;
    if (next == head) return 0;                      /* full: the BIOS would beep */
    wr16(c, 0x400u + tail, code);
    wr16(c, BDA_TAIL, next);
    return 1;
}

/* Like the real BIOS: records the lock state and sends it to the keyboard
 * (EDh + LED byte). The keyboard's FAh replies are ignored by INT 09h. */
static void update_leds(kbc_t *c)
{
    uint8_t f1 = rd8(c, BDA_FLAGS1);
    uint8_t leds = (uint8_t)(((f1 >> 4) & 1) | (((f1 >> 5) & 1) << 1) | (((f1 >> 6) & 1) << 2));
    wr8(c, BDA_LEDS, (uint8_t)((rd8(c, BDA_LEDS) & 0xF8) | leds));
    send_to_device(c, 0xED);
    send_to_device(c, leds);
}

static void toggle_lock(kbc_t *c, uint8_t held_bit, uint8_t lock_bit, int brk)
{
    if (brk) { setbit(c, BDA_FLAGS2, held_bit, 0); return; }
    if (rd8(c, BDA_FLAGS2) & held_bit) return;       /* still held: no toggle on repeats */
    setbit(c, BDA_FLAGS2, held_bit, 1);
    wr8(c, BDA_FLAGS1, (uint8_t)(rd8(c, BDA_FLAGS1) ^ lock_bit));
    update_leds(c);
}

void kbc_bios_irq(kbc_t *c)
{
    kbc_state_t *s = &c->st;
    uint8_t b, sc;
    int brk, ext;

    kbc_port_in(c, 0x60, &b);
    if (s->bios_skip) { s->bios_skip--; return; }
    if (b == 0xE1) { s->bios_skip = 5; return; }     /* Pause: E1 1D 45 E1 9D C5, ignored */
    if (b == 0xE0) { s->bios_e0 = 1; setbit(c, BDA_FLAGS3, 0x02, 1); return; }
    /* keyboard responses; AAh is not here: it is also the break code of left
     * Shift, and the real BIOS treats it as such (harmless after a reset) */
    if (b == 0xFA || b == 0xFE || b == 0xEE || b == 0x00 || b == 0xFF) return;

    sc = (uint8_t)(b & 0x7F);
    brk = (b & 0x80) != 0;
    ext = s->bios_e0;
    s->bios_e0 = 0;
    setbit(c, BDA_FLAGS3, 0x02, 0);

    switch (sc) {
    case 0x2A: if (!ext) setbit(c, BDA_FLAGS1, 0x02, !brk); return;      /* left Shift (E0 2A = fake shift) */
    case 0x36: if (!ext) setbit(c, BDA_FLAGS1, 0x01, !brk); return;      /* right Shift */
    case 0x1D:
        if (ext) setbit(c, BDA_FLAGS3, 0x04, !brk); else setbit(c, BDA_FLAGS2, 0x01, !brk);
        setbit(c, BDA_FLAGS1, 0x04, (rd8(c, BDA_FLAGS3) & 0x04) || (rd8(c, BDA_FLAGS2) & 0x01));
        return;
    case 0x38:
        if (ext) setbit(c, BDA_FLAGS3, 0x08, !brk); else setbit(c, BDA_FLAGS2, 0x02, !brk);
        setbit(c, BDA_FLAGS1, 0x08, (rd8(c, BDA_FLAGS3) & 0x08) || (rd8(c, BDA_FLAGS2) & 0x02));
        return;
    case 0x3A: if (!ext) toggle_lock(c, 0x40, 0x40, brk); return;         /* Caps Lock */
    case 0x45: if (!ext) toggle_lock(c, 0x20, 0x20, brk); return;         /* Num Lock */
    case 0x46: if (!ext) toggle_lock(c, 0x10, 0x10, brk); return;         /* Scroll Lock */
    default: break;
    }
    if (brk) {
        if (sc == 0x52) setbit(c, BDA_FLAGS2, 0x80, 0);
        return;
    }
    if (sc == 0x52) {
        /* Insert toggles insert mode unless it types a digit (keypad with Num Lock) */
        uint8_t f1 = rd8(c, BDA_FLAGS1);
        int digit = !ext && !(f1 & 0x0C) && (((f1 & 0x20) != 0) ^ ((f1 & 0x03) != 0));
        if (!digit && !(f1 & 0x0C) && !(rd8(c, BDA_FLAGS2) & 0x80)) {
            wr8(c, BDA_FLAGS1, (uint8_t)(f1 ^ 0x80));
            setbit(c, BDA_FLAGS2, 0x80, 1);
        }
    }
    {
        uint16_t code = translate(c, sc, ext);
        if (code) buffer_put(c, code);
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

static int buffer_peek(kbc_t *c, uint16_t *code)
{
    uint16_t head = rd16(c, BDA_HEAD);
    if (head == rd16(c, BDA_TAIL)) return 0;
    *code = rd16(c, 0x400u + head);
    return 1;
}

static void buffer_drop(kbc_t *c)
{
    uint16_t head = (uint16_t)(rd16(c, BDA_HEAD) + 2);
    if (head >= rd16(c, BDA_END)) head = rd16(c, BDA_START);
    wr16(c, BDA_HEAD, head);
}

/* A program can corrupt the buffer pointers in the BIOS data area; the loops
 * below are bounded so the host can never get stuck in them. */
#define MAX_SCAN 64

int kbc_int16(kbc_t *c, kbc_bios_regs_t *r)
{
    uint8_t ah = (uint8_t)(r->ax >> 8), al = (uint8_t)r->ax;
    uint16_t code;
    int guard;
    switch (ah) {
    case 0x00: case 0x10:
        for (guard = 0; guard < MAX_SCAN; guard++) {
            if (!buffer_peek(c, &code)) return KBC_BIOS_WAIT;
            buffer_drop(c);
            if (ah == 0x00) code = to_standard(code);
            if (code) { r->ax = code; return KBC_BIOS_DONE; }
        }
        return KBC_BIOS_WAIT;
    case 0x01: case 0x11:
        for (guard = 0; guard < MAX_SCAN; guard++) {
            if (!buffer_peek(c, &code)) { r->flags |= 0x40; return KBC_BIOS_DONE; }
            if (ah == 0x01) {
                uint16_t std = to_standard(code);
                if (!std) { buffer_drop(c); continue; }
                code = std;
            }
            r->ax = code;
            r->flags &= (uint16_t)~0x40;
            return KBC_BIOS_DONE;
        }
        r->flags |= 0x40;
        return KBC_BIOS_DONE;
    case 0x02:
        r->ax = (uint16_t)((r->ax & 0xFF00) | rd8(c, BDA_FLAGS1));
        return KBC_BIOS_DONE;
    case 0x12: {
        uint8_t f2 = rd8(c, BDA_FLAGS2), f3 = rd8(c, BDA_FLAGS3);
        uint8_t hi = (uint8_t)((f2 & 0x01) | (f2 & 0x02) | (f3 & 0x04) | (f3 & 0x08) |
                               (f2 & 0x10) | (f2 & 0x20) | (f2 & 0x40) | ((f2 & 0x04) << 5));
        r->ax = (uint16_t)((hi << 8) | rd8(c, BDA_FLAGS1));
        return KBC_BIOS_DONE;
    }
    case 0x03:
        if (al == 0x05) {
            uint8_t delay = (uint8_t)(r->bx >> 8), rate = (uint8_t)r->bx;
            if (delay > 3 || rate > 0x1F) return KBC_BIOS_UNSUPPORTED;
            send_to_device(c, 0xF3);
            send_to_device(c, (uint8_t)((delay << 5) | rate));
            return KBC_BIOS_DONE;
        }
        if (al == 0x00) { send_to_device(c, 0xF3); send_to_device(c, 0x2B); return KBC_BIOS_DONE; }
        return KBC_BIOS_UNSUPPORTED;
    case 0x05:
        r->ax = (uint16_t)((r->ax & 0xFF00) | (buffer_put(c, r->cx) ? 0 : 1));
        return KBC_BIOS_DONE;
    default:
        return KBC_BIOS_UNSUPPORTED;
    }
}

/* ---------- lifecycle ---------- */

kbc_t *kbc_create(uint8_t *memory)
{
    kbc_t *c;
    if (!memory) return NULL;
    c = (kbc_t *)calloc(1, sizeof(kbc_t));
    if (!c) return NULL;
    c->mem = memory;
    kbc_reset(c);
    return c;
}

void kbc_destroy(kbc_t *c) { free(c); }

void kbc_set_memory(kbc_t *c, uint8_t *memory) { if (memory) c->mem = memory; }

/* Reads and drops everything the keyboard has queued (the POST does this). */
static void discard_input(kbc_t *c)
{
    uint8_t b;
    int guard = 0;
    while (c->has_dev && c->dev.read && guard++ < 64 && c->dev.read(c->dev.user, &b)) {}
}

void kbc_reset(kbc_t *c)
{
    memset(&c->st, 0, sizeof(c->st));
    c->st.cmd_byte = 0x45;        /* IRQ 1 enabled, system flag, scan code translation */
    wr8(c, BDA_FLAGS1, 0x20);     /* Num Lock on at boot */
    wr8(c, BDA_FLAGS2, 0);
    wr16(c, BDA_HEAD, 0x1E);
    wr16(c, BDA_TAIL, 0x1E);
    wr16(c, BDA_START, 0x1E);
    wr16(c, BDA_END, 0x3E);
    wr8(c, BDA_FLAGS3, 0x10);     /* enhanced (101-key) keyboard */
    wr8(c, BDA_LEDS, 0);
    discard_input(c);
    if (c->has_dev) update_leds(c);
    discard_input(c);
    memset(&c->st, 0, sizeof(c->st));
    c->st.cmd_byte = 0x45;
}

size_t kbc_state_size(void) { return sizeof(kbc_state_t); }

size_t kbc_save_state(const kbc_t *c, void *buf, size_t len)
{
    if (len < sizeof(kbc_state_t)) return 0;
    memcpy(buf, &c->st, sizeof(kbc_state_t));
    return sizeof(kbc_state_t);
}

int kbc_load_state(kbc_t *c, const void *buf, size_t len)
{
    if (len != sizeof(kbc_state_t)) return -1;
    memcpy(&c->st, buf, sizeof(kbc_state_t));
    return 0;
}
