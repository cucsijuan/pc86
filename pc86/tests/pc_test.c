/*
 * Machine tests for pc86: a whole PC (CPU, board chips, VGA card, monitor,
 * keyboard) running small programs.
 */
#include "asm86.h"
#include "pc.h"
#include "pc_vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

#define FRAME (1.0 / 60.0)

static pc_t *pc;
static pc_vga_card_t *vga;
static monitor_t *mono, *color, *cga;
static kbd_t *kbd;
static uint8_t screen[640 * 400 * 3];

/* Minimal DOS: INT 20h and INT 21h AH=4Ch end the program. */
static int dos(void *user, pc_t *p, uint8_t n)
{
    vm86_regs_t *r = vm86_regs(pc_cpu(p));
    (void)user;
    if (n == 0x20 || (n == 0x21 && (r->ax >> 8) == 0x4C)) { pc_stop(p); return 1; }
    return 0;
}

static asm86_symbol_t syms[256];
static int nsyms;

/* Assembles and loads a program; keeps its symbols for sym(). */
static void load(const char *src)
{
    asm86_result_t a;
    int i;
    if (asm86_assemble(src, &a) != 0) {
        printf("assembly error line %d: %s\n", a.errors[0].line, a.errors[0].message);
        exit(1);
    }
    nsyms = a.symbol_count < 256 ? a.symbol_count : 256;
    for (i = 0; i < nsyms; i++) syms[i] = a.symbols[i];
    if (pc_load_com(pc, a.code, a.size) != 0) { printf("cannot load program\n"); exit(1); }
    asm86_free(&a);
}

static uint16_t var(const char *name)
{
    int i;
    uint8_t *m = pc_memory(pc) + (PC_COM_SEGMENT << 4);
    for (i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, name)) return (uint16_t)(m[syms[i].value] | m[syms[i].value + 1] << 8);
    printf("no symbol %s\n", name);
    exit(1);
}

/* Runs up to max_frames engine frames; returns how many ran until it stopped. */
static int run_frames(int max_frames)
{
    int f;
    for (f = 0; f < max_frames; f++) {
        pc_status_t st = pc_status(pc);
        if (st != PC_RUNNING && st != PC_HALTED) break;
        pc_run(pc, FRAME);
    }
    return f;
}

static void boot(void)
{
    pc_power_off(pc);
    pc_power_on(pc);
    run_frames(3);
}

static void type(uint16_t key)
{
    kbd_key(kbd, key, 1); run_frames(2);
    kbd_key(kbd, key, 0); run_frames(2);
}

static int text_at(int col, int row, const char *s)
{
    const uint8_t *m = pc_memory(pc) + 0xB8000 + (row * 80 + col) * 2;
    for (; *s; s++, m += 2) if (*m != (uint8_t)*s) return 0;
    return 1;
}

int main(void)
{
    uint8_t *state;
    size_t len;
    int f;

    pc = pc_create();
    vga = pc_vga_card_create();
    mono = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_MONO));
    color = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_COLOR));
    cga = monitor_create(monitor_builtin_model(MONITOR_MODEL_CGA_COLOR));
    kbd = kbd_create(kbd_builtin_model(KBD_MODEL_ENHANCED101));
    CHECK(pc && vga && mono && kbd, "machine, card, monitor and keyboard created");
    pc_set_int_hook(pc, dos, NULL);

    /* ---------- assembling the machine ---------- */
    CHECK(pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == 0, "VGA card inserted in slot 0");
    CHECK(pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == -1, "a slot holds one card");
    CHECK(pc_vga_card_connect_monitor(vga, cga) == -1, "a CGA monitor does not fit the VGA connector");
    CHECK(pc_vga_card_connect_monitor(vga, mono) == 0, "a mono VGA monitor does");
    pc_connect_keyboard(pc, kbd);
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_NO_SIGNAL, "machine off: the monitor has no signal");

    /* ---------- power on ---------- */
    pc_power_on(pc);
    CHECK(pc_insert_card(pc, 1, pc_vga_card_slot(vga)) == -1 && pc_remove_card(pc, 0) == NULL,
          "cards cannot be inserted or removed with the power on");
    CHECK(kbd_powered(kbd) && kbd_leds(kbd) == KBD_LED_NUM, "the keyboard gets power; the POST lights Num Lock");
    run_frames(6);
    CHECK(pc_status(pc) == PC_HALTED && text_at(0, 0, "No boot device available."),
          "with no storage the BIOS says so and waits in HLT");
    {
        int lit = 0, i;
        CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_SHOWING, "the monitor is showing the card's signal");
        for (i = 0; i < 640 * 16; i++) lit += screen[i] > 100;
        CHECK(lit > 200, "and the first text row has the BIOS message on it");
    }
    {
        uint8_t *m = pc_memory(pc);
        uint32_t t0 = m[0x46C] | m[0x46D] << 8;
        run_frames(60);
        f = (int)((m[0x46C] | m[0x46D] << 8) - t0);
        CHECK(f == 18 || f == 19, "the BIOS timer interrupt counts 18.2 ticks per second while idle");
    }

    /* ---------- a program hooking the user timer interrupt ---------- */
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:1Ch*4], tick\n"
         "    mov [es:1Ch*4+2], cs\n"
         "    sti\n"
         "wait_ticks:\n"
         "    hlt\n"
         "    cmp word [count], 18\n"
         "    jb wait_ticks\n"
         "    int 20h\n"
         "tick:\n"
         "    inc word [cs:count]\n"
         "    iret\n"
         "count dw 0\n");
    f = run_frames(120);
    CHECK(pc_status(pc) == PC_STOPPED && var("count") == 18 && f >= 58 && f <= 61,
          "INT 1Ch hook: 18 ticks take one second of emulated time");

    /* ---------- reprogramming the PIT ---------- */
    boot();
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:8*4], tick\n"
         "    mov [es:8*4+2], cs\n"
         "    mov al, 34h\n"
         "    out 43h, al\n"
         "    mov ax, 1193\n"
         "    out 40h, al\n"
         "    mov al, ah\n"
         "    out 40h, al\n"
         "    sti\n"
         "wait_ticks:\n"
         "    hlt\n"
         "    cmp word [count], 1000\n"
         "    jb wait_ticks\n"
         "    int 20h\n"
         "tick:\n"
         "    push ax\n"
         "    inc word [cs:count]\n"
         "    mov al, 20h\n"
         "    out 20h, al\n"
         "    pop ax\n"
         "    iret\n"
         "count dw 0\n");
    f = run_frames(120);
    CHECK(pc_status(pc) == PC_STOPPED && f >= 59 && f <= 61, "own INT 08h with the PIT at 1000 Hz: 1000 ticks in one second");

    /* ---------- BIOS services ---------- */
    boot();
    run_frames(30);
    load("org 100h\n"
         "    int 12h\n"
         "    mov [mem], ax\n"
         "    xor ah, ah\n"
         "    int 1Ah\n"
         "    mov [ticks], dx\n"
         "    mov ax, 1A00h\n"
         "    int 10h\n"
         "    mov [dcc], bx\n"
         "    int 20h\n"
         "mem dw 0\n"
         "ticks dw 0\n"
         "dcc dw 0\n");
    run_frames(2);
    CHECK(var("mem") == 640, "INT 12h: 640 KB");
    CHECK(var("ticks") >= 9 && var("ticks") <= 11, "INT 1Ah AH=00h: the tick count since power on");
    CHECK((var("dcc") & 0xFF) == 0x07, "INT 10h AX=1A00h: monochrome VGA display (the monitor's ID pins)");
    pc_vga_card_connect_monitor(vga, color);
    load("org 100h\n"
         "    mov ax, 1A00h\n"
         "    int 10h\n"
         "    mov [dcc], bx\n"
         "    int 20h\n"
         "dcc dw 0\n");
    run_frames(2);
    CHECK((var("dcc") & 0xFF) == 0x08, "after hot-swapping to a color monitor: color VGA display");
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_SHOWING, "and the color monitor shows the picture");
    pc_vga_card_connect_monitor(vga, NULL);
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == -1, "with the monitor unplugged there is nothing to show");
    pc_vga_card_connect_monitor(vga, mono);

    /* ---------- keyboard ---------- */
    load("org 100h\n"
         "    xor ah, ah\n"
         "    int 16h\n"
         "    mov [key1], ax\n"
         "    xor ah, ah\n"
         "    int 16h\n"
         "    mov [key2], ax\n"
         "    int 20h\n"
         "key1 dw 0\n"
         "key2 dw 0\n");
    run_frames(3);
    type(0x1E);
    CHECK(var("key1") == 0x1E61, "INT 16h gets 'a' from the keyboard");
    pc_connect_keyboard(pc, NULL);
    CHECK(!kbd_powered(kbd), "unplugging the keyboard cuts its power");
    type(0x30);
    CHECK(pc_status(pc) == PC_RUNNING && var("key2") == 0, "keys on an unplugged keyboard go nowhere");
    pc_connect_keyboard(pc, kbd);
    run_frames(3);
    type(0x30);
    CHECK(pc_status(pc) == PC_STOPPED && var("key2") == 0x3062, "plugged back in while running: 'b' arrives");

    pc_connect_keyboard(pc, NULL);
    load("org 100h\n"
         "    mov al, 0EDh\n"
         "    out 60h, al\n"
         "    mov cx, 100\n"
         "d1: loop d1\n"
         "    in al, 64h\n"
         "    mov [status], al\n"
         "    int 20h\n"
         "status db 0, 0\n");
    run_frames(2);
    CHECK((var("status") & 0x40) != 0, "a command with no keyboard times out (port 64h bit 6)");
    pc_connect_keyboard(pc, kbd);

    /* ---------- port 61h: speaker and refresh ---------- */
    load("org 100h\n"
         "    mov al, 0B6h\n"
         "    out 43h, al\n"
         "    mov ax, 1193\n"
         "    out 42h, al\n"
         "    mov al, ah\n"
         "    out 42h, al\n"
         "    in al, 61h\n"
         "    or al, 3\n"
         "    out 61h, al\n"
         "    mov cx, 200\n"
         "    in al, 61h\n"
         "    and al, 10h\n"
         "    mov bl, al\n"
         "poll:\n"
         "    in al, 61h\n"
         "    and al, 10h\n"
         "    cmp al, bl\n"
         "    je same\n"
         "    inc word [toggles]\n"
         "    mov bl, al\n"
         "same:\n"
         "    loop poll\n"
         "    hlt\n"
         "    jmp $-1\n"
         "toggles dw 0\n");
    run_frames(3);
    CHECK(pc_speaker_hz(pc) > 999.0 && pc_speaker_hz(pc) < 1001.0, "the speaker plays the 1000 Hz tone of PIT channel 2");
    CHECK(var("toggles") > 10, "port 61h bit 4 toggles with the DRAM refresh");

    /* ---------- the bus ---------- */
    load("org 100h\n"
         "    mov dx, 378h\n"
         "    in al, dx\n"
         "    mov [v], al\n"
         "    out dx, al\n"
         "    int 20h\n"
         "v db 0, 0\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_STOPPED && (var("v") & 0xFF) == 0xFF, "a port nobody decodes reads FFh (no printer card)");
    load("org 100h\n"
         "    mov dx, 3C4h\n"
         "    out dx, al\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_ERROR && strstr(pc_error(pc), "3C4") != NULL, "a VGA port the card does not emulate is reported");
    printf("     (%s)\n", pc_error(pc));
    boot();
    load("org 100h\n"
         "    mov ax, 4F00h\n"
         "    int 10h\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_ERROR && strstr(pc_error(pc), "INT 10h") != NULL, "an unsupported video BIOS call is reported");
    printf("     (%s)\n", pc_error(pc));

    /* ---------- save and load ---------- */
    boot();
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:1Ch*4], tick\n"
         "    mov [es:1Ch*4+2], cs\n"
         "    sti\n"
         "forever:\n"
         "    hlt\n"
         "    jmp forever\n"
         "tick:\n"
         "    inc word [cs:count]\n"
         "    iret\n"
         "count dw 0\n");
    run_frames(30);
    len = pc_state_size(pc);
    state = (uint8_t *)malloc(len);
    {
        uint16_t saved = var("count");
        CHECK(pc_save_state(pc, state, len) == len, "machine state saved");
        run_frames(60);
        CHECK(var("count") > saved && pc_load_state(pc, state, len) == 0 && var("count") == saved, "and loaded back");
        run_frames(60);
        CHECK(var("count") >= saved + 18, "it keeps running from there");
    }
    free(state);

    /* ---------- a machine without a video card ---------- */
    pc_power_off(pc);
    pc_remove_card(pc, 0);
    pc_power_on(pc);
    run_frames(3);
    load("org 100h\n"
         "    mov ax, 0E41h\n"
         "    int 10h\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_STOPPED, "without a video card INT 10h does nothing and the program goes on");
    pc_power_off(pc);
    CHECK(!kbd_powered(kbd) && pc_status(pc) == PC_OFF, "power off: the keyboard goes dark");

    pc_destroy(pc);
    pc_vga_card_destroy(vga);
    monitor_destroy(mono); monitor_destroy(color); monitor_destroy(cga);
    kbd_destroy(kbd);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
