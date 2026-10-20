/* Unit tests for kbd86 (keyboard, controller + BIOS) and pic86. They drive the devices directly (no CPU). */
#include "kbc.h"
#include "kbd.h"
#include "pic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static uint8_t mem[1 << 20];
static kbc_t *C;     /* the controller the keyboard is plugged into */

/* Reads every byte the keyboard has ready on port 60h. */
static int drain(uint8_t *out, int max)
{
    int n = 0;
    while (kbc_take_irq(C) && n < max) kbc_port_in(C, 0x60, &out[n++]);
    return n;
}

/* Delivers every pending byte to the BIOS INT 09h handler. */
static void bios(void) { while (kbc_take_irq(C)) kbc_bios_irq(C); }

static void tap(kbd_t *k, uint16_t key) { kbd_key(k, key, 1); bios(); kbd_key(k, key, 0); bios(); }

static uint16_t get_key(uint8_t fn)
{
    kbc_bios_regs_t r;
    memset(&r, 0, sizeof(r));
    r.ax = (uint16_t)(fn << 8);
    return kbc_int16(C, &r) == KBC_BIOS_DONE ? r.ax : 0xFFFF;
}

static uint16_t key_with(kbd_t *k, uint16_t modifier, uint16_t key, uint8_t fn)
{
    kbd_key(k, modifier, 1); bios();
    tap(k, key);
    kbd_key(k, modifier, 0); bios();
    return get_key(fn);
}

int main(void)
{
    kbd_t *k = kbd_create(NULL);
    kbc_device_t dev = kbc_keyboard_device(k);
    uint8_t b[64];
    int n;
    kbc_bios_regs_t r;

    C = kbc_create(mem);
    kbc_connect(C, &dev);
    kbd_power(k, 1);
    CHECK(kbc_take_irq(C) && (kbc_port_in(C, 0x60, &b[0]), b[0] == 0xAA), "plugging the keyboard in: it sends AAh (self test passed)");
    kbc_reset(C);
    CHECK(kbc_take_irq(C) == 0, "the POST discards the keyboard's replies");

    /* ---------- scan codes on port 60h ---------- */
    kbd_key(k, kbd_key_from_code("KeyA"), 1);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x1E, "pressing A sends make code 1Eh");
    kbd_key(k, kbd_key_from_code("KeyA"), 0);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x9E, "releasing A sends break code 9Eh");
    kbd_key(k, kbd_key_from_code("ArrowUp"), 1);
    kbd_key(k, kbd_key_from_code("ArrowUp"), 0);
    n = drain(b, 64);
    CHECK(n == 4 && b[0] == 0xE0 && b[1] == 0x48 && b[2] == 0xE0 && b[3] == 0xC8, "extended key: E0 48 / E0 C8");
    kbd_key(k, 0x1E, 1);
    kbd_key(k, 0x1E, 1);
    n = drain(b, 64);
    CHECK(n == 1, "a second press without release (host auto-repeat) is ignored");
    kbd_key(k, 0x1E, 0); drain(b, 64);

    /* status port: output buffer full bit */
    kbd_key(k, 0x1F, 1);
    kbc_port_in(C, 0x64, &b[0]);
    CHECK((b[0] & 0x01) == 1 && (b[0] & 0x10), "port 64h: output buffer full, keyboard enabled");
    kbc_port_in(C, 0x60, &b[1]);
    kbc_port_in(C, 0x64, &b[0]);
    CHECK((b[0] & 0x01) == 0, "reading port 60h empties the output buffer");
    CHECK(kbc_take_irq(C) == 1 && kbc_take_irq(C) == 0, "one IRQ per scan code");
    kbd_key(k, 0x1F, 0); drain(b, 64);

    /* ---------- typematic repeat ---------- */
    kbd_key(k, 0x1E, 1);
    drain(b, 64);
    kbd_advance(k, 499000);
    CHECK(drain(b, 64) == 0, "no repeat before the 500 ms delay");
    kbd_advance(k, 2000);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x1E, "first repeat after 500 ms");
    kbd_advance(k, 1000000);
    n = drain(b, 64);
    CHECK(n >= 10 && n <= 12, "about 10.9 repeats per second afterwards");
    kbd_key(k, 0x1E, 0);
    drain(b, 64);
    kbd_advance(k, 1000000);
    CHECK(drain(b, 64) == 0, "releasing the key stops the repeat");

    /* ---------- keyboard and controller commands ---------- */
    kbc_port_out(C, 0x60, 0xED);
    kbc_port_out(C, 0x60, 0x05);
    n = drain(b, 64);
    CHECK(n == 2 && b[0] == 0xFA && b[1] == 0xFA && kbd_leds(k) == (KBD_LED_SCROLL | KBD_LED_CAPS), "command EDh sets the LEDs, acknowledged with FAh");
    kbc_port_out(C, 0x60, 0xF2);
    n = drain(b, 64);
    CHECK(n == 3 && b[0] == 0xFA && b[1] == 0xAB && b[2] == 0x83, "command F2h identifies an MF2 keyboard (AB 83)");
    kbc_port_out(C, 0x60, 0xEE);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0xEE, "command EEh echoes");
    kbc_port_out(C, 0x60, 0xFF);
    n = drain(b, 64);
    CHECK(n == 2 && b[0] == 0xFA && b[1] == 0xAA, "reset: FA then self-test passed AA");
    kbc_port_out(C, 0x60, 0xF3);
    kbc_port_out(C, 0x60, 0x00);    /* 250 ms, 30 cps */
    drain(b, 64);
    kbd_key(k, 0x1E, 1); drain(b, 64);
    kbd_advance(k, 251000);
    CHECK(drain(b, 64) == 1, "command F3h changes the typematic delay");
    kbd_key(k, 0x1E, 0); drain(b, 64);
    kbc_port_out(C, 0x60, 0xF5); drain(b, 64);
    kbd_key(k, 0x1E, 1);
    CHECK(drain(b, 64) == 0, "after F5h (disable) keys are not sent");
    kbd_key(k, 0x1E, 0);
    kbc_port_out(C, 0x60, 0xF4); drain(b, 64);
    kbc_port_out(C, 0x64, 0xAA);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x55, "controller self test (64h <- AAh) returns 55h");
    kbc_port_out(C, 0x64, 0x20);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x45, "controller command byte reads 45h");
    kbc_port_out(C, 0x64, 0xAD);
    kbd_key(k, 0x1E, 1);
    CHECK(drain(b, 64) == 0, "64h <- ADh disables the keyboard interface");
    kbc_port_out(C, 0x64, 0xAE);
    n = drain(b, 64);
    CHECK(n == 1 && b[0] == 0x1E, "64h <- AEh enables it again and the pending code arrives");
    kbd_key(k, 0x1E, 0); drain(b, 64);
    CHECK(kbc_port_out(C, 0x64, 0xD1) == KBC_PORT_UNSUPPORTED, "controller output port (A20) is reported as unsupported");
    CHECK(kbc_port_out(C, 0x60, 0xF0) == KBC_PORT_UNSUPPORTED, "scan code set selection is reported as unsupported");
    CHECK(kbc_port_out(C, 0x61, 0) == KBC_PORT_NOT_MINE && kbc_port_in(C, 0x20, &b[0]) == KBC_PORT_NOT_MINE, "other ports are left to other devices");

    /* ---------- BIOS translation ---------- */
    kbc_reset(C);
    CHECK(mem[0x417] == 0x20 && kbd_leds(k) == KBD_LED_NUM, "boot: Num Lock on, its LED lit");
    tap(k, 0x1E);
    CHECK(get_key(0x00) == 0x1E61, "a -> 1E61h");
    CHECK(key_with(k, 0x2A, 0x1E, 0x00) == 0x1E41, "Shift+a -> 1E41h ('A')");
    CHECK(key_with(k, 0x1D, 0x2E, 0x00) == 0x2E03, "Ctrl+c -> 2E03h");
    CHECK(key_with(k, 0x38, 0x2D, 0x00) == 0x2D00, "Alt+x -> 2D00h");
    CHECK(key_with(k, 0x2A, 0x03, 0x00) == 0x0340, "Shift+2 -> '@' (US layout)");
    tap(k, 0x3B);
    CHECK(get_key(0x00) == 0x3B00, "F1 -> 3B00h");
    CHECK(key_with(k, 0x2A, 0x44, 0x00) == 0x5D00, "Shift+F10 -> 5D00h");
    tap(k, 0x1C);
    CHECK(get_key(0x00) == 0x1C0D, "Enter -> 1C0Dh");
    tap(k, 0x01);
    CHECK(get_key(0x00) == 0x011B, "Esc -> 011Bh");
    tap(k, 0x39);
    CHECK(get_key(0x00) == 0x3920, "Space -> 3920h");

    tap(k, 0x3A);   /* Caps Lock on */
    CHECK((mem[0x417] & 0x40) && (kbd_leds(k) & KBD_LED_CAPS), "Caps Lock toggles its flag and LED");
    tap(k, 0x1E);
    CHECK(get_key(0x00) == 0x1E41, "Caps Lock: a -> 'A'");
    CHECK(key_with(k, 0x2A, 0x1E, 0x00) == 0x1E61, "Caps Lock + Shift: a -> 'a'");
    tap(k, 0x02);
    CHECK(get_key(0x00) == 0x0231, "Caps Lock does not affect digits");
    tap(k, 0x3A);
    CHECK(!(mem[0x417] & 0x40) && !(kbd_leds(k) & KBD_LED_CAPS), "Caps Lock off again");

    /* keypad and gray keys */
    tap(k, 0x48);
    CHECK(get_key(0x00) == 0x4838, "keypad 8 with Num Lock -> '8'");
    tap(k, 0x45);   /* Num Lock off */
    tap(k, 0x48);
    CHECK(get_key(0x00) == 0x4800, "keypad 8 without Num Lock -> Up (4800h)");
    tap(k, 0xE048);
    CHECK(get_key(0x10) == 0x48E0, "gray Up via AH=10h -> 48E0h");
    tap(k, 0xE048);
    CHECK(get_key(0x00) == 0x4800, "gray Up via AH=00h -> 4800h");
    tap(k, 0xE01C);
    CHECK(get_key(0x00) == 0x1C0D && (tap(k, 0xE01C), get_key(0x10) == 0xE00D), "keypad Enter: 1C0Dh standard, E00Dh enhanced");
    tap(k, 0x57);
    tap(k, 0x1E);
    CHECK(get_key(0x00) == 0x1E61, "F11 is skipped by the standard function AH=00h");
    tap(k, 0x57);
    CHECK(get_key(0x10) == 0x8500, "F11 via AH=10h -> 8500h");
    CHECK(key_with(k, 0xE01D, 0xE04D, 0x10) == 0x74E0, "right Ctrl + gray Right -> 74E0h");
    tap(k, 0x45);   /* Num Lock back on */

    /* Insert toggles insert mode */
    tap(k, 0xE052);
    CHECK((mem[0x417] & 0x80) && get_key(0x10) == 0x52E0, "gray Insert toggles insert mode and is buffered");
    tap(k, 0xE052);
    get_key(0x10);
    CHECK(!(mem[0x417] & 0x80), "pressing Insert again turns it off");

    /* shift flags */
    kbd_key(k, 0x2A, 1); bios();
    kbd_key(k, 0xE038, 1); bios();
    memset(&r, 0, sizeof(r));
    r.ax = 0x0200;
    kbc_int16(C, &r);
    CHECK((r.ax & 0xFF) == (0x02 | 0x08 | 0x20), "AH=02h: left Shift, Alt and Num Lock");
    r.ax = 0x1200;
    kbc_int16(C, &r);
    CHECK((r.ax >> 8) == 0x08, "AH=12h: right Alt held");
    kbd_key(k, 0x2A, 0); bios();
    kbd_key(k, 0xE038, 0); bios();
    r.ax = 0x0200;
    kbc_int16(C, &r);
    CHECK((r.ax & 0xFF) == 0x20, "flags clear on release");

    /* AH=01h peek, ZF, wait */
    memset(&r, 0, sizeof(r));
    r.ax = 0x0100;
    CHECK(kbc_int16(C, &r) == KBC_BIOS_DONE && (r.flags & 0x40), "AH=01h with an empty buffer: ZF=1");
    r.ax = 0x0000;
    CHECK(kbc_int16(C, &r) == KBC_BIOS_WAIT, "AH=00h with an empty buffer asks the host to wait");
    tap(k, 0x10);
    r.ax = 0x0100; r.flags = 0x40;
    kbc_int16(C, &r);
    CHECK(!(r.flags & 0x40) && r.ax == 0x1071, "AH=01h: ZF=0 and the key, without removing it");
    CHECK(get_key(0x00) == 0x1071, "the key is still there for AH=00h");

    /* buffer limits and AH=05h */
    {
        int i, stored = 0;
        for (i = 0; i < 20; i++) { r.ax = 0x0500; r.cx = (uint16_t)(0x1E61 + i); kbc_int16(C, &r); if ((r.ax & 0xFF) == 0) stored++; }
        CHECK(stored == 15, "the BIOS buffer holds 15 keys (AH=05h reports full afterwards)");
        CHECK(get_key(0x00) == 0x1E61 && get_key(0x00) == 0x1E62, "keys come out in order");
        while (get_key(0x10) != 0xFFFF) {}
    }
    memset(&r, 0, sizeof(r));
    r.ax = 0x0305; r.bx = 0x0000;
    CHECK(kbc_int16(C, &r) == KBC_BIOS_DONE, "AX=0305h sets the typematic rate");
    r.ax = 0x0A00;
    CHECK(kbc_int16(C, &r) == KBC_BIOS_UNSUPPORTED, "unsupported INT 16h functions are reported");

    /* repeated make codes produce repeated keys */
    kbd_key(k, 0x1E, 1); bios();
    kbd_advance(k, 260000); bios();    /* typematic is 250 ms / 30 cps now */
    kbd_key(k, 0x1E, 0); bios();
    CHECK(get_key(0x00) == 0x1E61 && get_key(0x00) == 0x1E61, "typematic repeats reach the BIOS buffer");
    while (get_key(0x10) != 0xFFFF) {}

    /* release_all */
    kbd_key(k, 0x2A, 1); kbd_key(k, 0xE04B, 1); bios();
    kbd_release_all(k); bios();
    CHECK(!(mem[0x417] & 0x03), "kbd_release_all releases held keys (no stuck Shift)");

    /* save / load */
    {
        size_t sz = kbd_state_size();
        void *buf = malloc(sz);
        kbd_t *k2 = kbd_create(NULL);
        kbc_port_out(C, 0x60, 0xED); kbc_port_out(C, 0x60, 0x07); drain(b, 64);
        CHECK(kbd_save_state(k, buf, sz) == sz && kbd_load_state(k2, buf, sz) == 0 && kbd_leds(k2) == 7, "state saved and loaded");
        free(buf);
        kbd_destroy(k2);
    }


    /* ---------- models ---------- */
    {
        kbd_t *at = kbd_create(kbd_builtin_model(KBD_MODEL_AT84));
        kbc_device_t d2 = kbc_keyboard_device(at);
        kbc_connect(C, &d2);
        kbd_power(at, 1);
        drain(b, 64);
        kbd_key(at, 0x57, 1); kbd_key(at, 0xE048, 1);
        CHECK(drain(b, 64) == 0, "an 84-key AT keyboard has no F11 or gray arrows");
        kbd_key(at, 0x48, 1);
        n = drain(b, 64);
        CHECK(n == 1 && b[0] == 0x48, "but it has the keypad arrows");
        kbc_port_out(C, 0x60, 0xF2);
        n = drain(b, 64);
        CHECK(n == 1 && b[0] == 0xFA, "F2h on an AT keyboard: only FAh, no ID bytes");
        CHECK(kbd_has_key(kbd_builtin_model(KBD_MODEL_ENHANCED102), 0x56) && !kbd_has_key(kbd_builtin_model(KBD_MODEL_ENHANCED101), 0x56),
              "the ISO key exists only on 102-key models");
        CHECK(kbd_has_key(kbd_builtin_model(KBD_MODEL_WINDOWS104), 0xE05B) && !kbd_has_key(kbd_builtin_model(KBD_MODEL_ENHANCED101), 0xE05B),
              "Windows keys only on the 104-key model");

        /* ---------- unplugging and plugging ---------- */
        kbc_connect(C, NULL);
        kbd_power(at, 0);
        kbd_key(at, 0x1E, 1);
        CHECK(drain(b, 64) == 0 && kbd_leds(at) == 0, "unplugged: keys go nowhere and the LEDs are dark");
        kbc_port_out(C, 0x60, 0xED);
        kbc_port_in(C, 0x64, &b[1]);
        n = drain(b, 64);
        CHECK(n == 1 && b[0] == 0xFE && (b[1] & 0x40), "a command with nothing plugged in times out: FEh and status bit 6");
        kbc_port_out(C, 0x64, 0xAB);
        n = drain(b, 64);
        CHECK(n == 1 && b[0] == 0x02, "interface test (ABh) reports the clock line stuck low");
        kbd_destroy(at);
        kbc_connect(C, &dev);
        kbd_power(k, 0);
        kbd_power(k, 1);
        n = drain(b, 64);
        CHECK(n == 1 && b[0] == 0xAA, "hot-plugging the keyboard back: AAh arrives");
        kbc_port_in(C, 0x64, &b[1]);
        CHECK(!(b[1] & 0x40), "and the time-out bit is clear");
    }

    /* key names */
    CHECK(kbd_key_from_code("KeyZ") == 0x2C && kbd_key_from_code("NumpadEnter") == 0xE01C &&
          kbd_key_from_code("Semicolon") == 0x27 && kbd_key_from_code("Nope") == 0, "W3C key code names");

    /* ---------- PIC ---------- */
    {
        pic_t p;
        uint8_t v;
        pic_reset(&p);
        pic_raise(&p, 1);
        CHECK(pic_acknowledge(&p) == 0x09, "IRQ 1 is delivered as INT 09h");
        CHECK(pic_acknowledge(&p) == -1, "nothing else while it is in service");
        pic_raise(&p, 1);
        CHECK(pic_acknowledge(&p) == -1, "a new IRQ 1 waits for the EOI");
        pic_port_out(&p, 0x20, 0x20);
        CHECK(pic_acknowledge(&p) == 0x09, "non-specific EOI (20h) lets it through");
        pic_raise(&p, 0);
        CHECK(pic_acknowledge(&p) == 0x08, "IRQ 0 has higher priority and interrupts IRQ 1 in service");
        pic_port_out(&p, 0x20, 0x0B);
        pic_port_in(&p, 0x20, &v);
        CHECK(v == 0x03, "OCW3 0Bh: reading ISR shows IRQ 0 and 1 in service");
        pic_port_out(&p, 0x20, 0x60);   /* specific EOI for IRQ 0 */
        pic_port_in(&p, 0x20, &v);
        CHECK(v == 0x02, "specific EOI (60h) clears IRQ 0 only");
        pic_eoi(&p);
        pic_port_out(&p, 0x21, 0xFF);
        pic_raise(&p, 1);
        CHECK(pic_acknowledge(&p) == -1, "masked IRQs are not delivered");
        pic_port_out(&p, 0x21, 0xFC);
        CHECK(pic_acknowledge(&p) == 0x09, "unmasking delivers the waiting request");
        pic_eoi(&p);
        /* reprogram like an AT BIOS: ICW1 11h, ICW2 vector base, ICW3, ICW4 */
        pic_port_out(&p, 0x20, 0x11);
        pic_port_out(&p, 0x21, 0x20);
        pic_port_out(&p, 0x21, 0x04);
        pic_port_out(&p, 0x21, 0x01);
        pic_port_out(&p, 0x21, 0xFD);
        pic_raise(&p, 1);
        CHECK(pic_acknowledge(&p) == 0x21, "reinitialized with vector base 20h: IRQ 1 -> INT 21h");
        CHECK(pic_port_out(&p, 0x20, 0xA0) == PIC_PORT_UNSUPPORTED, "rotating priority is reported as unsupported");
        CHECK(pic_port_out(&p, 0x60, 0) == PIC_PORT_NOT_MINE, "other ports are left alone");
    }

    kbd_destroy(k);
    kbc_destroy(C);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
