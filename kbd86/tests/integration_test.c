/*
 * Integration tests with the vm86 CPU core and assembler (separate package).
 * Build with: make test-integration VM86_DIR=../vm86
 *
 * The rig below is also the reference for wiring keyboard + controller + PIC + CPU
 * (pc86 does the same for a whole machine):
 *   - INT 09h and INT 16h vectors point at BIOS "ROM" addresses; while they
 *     still do, the host services them in C. A program that installs its own
 *     handler changes the vector and gets the real behavior.
 *   - Between CPU slices: advance the keyboard clock, forward its IRQ to the
 *     PIC, and deliver the PIC's interrupt to the CPU if IF=1.
 */
#include "asm86.h"
#include "kbc.h"
#include "kbd.h"
#include "pic.h"
#include "vm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

#define BIOS_INT09 0xF000E987u   /* segment:offset of the BIOS handlers, as on a real PC */
#define BIOS_INT16 0xF000E82Eu
#define IPS 400000u
#define SLICE (IPS / 25000u)     /* 40 us */

typedef struct {
    vm86_t *cpu;
    kbd_t *kbd;
    kbc_t *kbc;
    pic_t pic;
    int bad_port;
} rig_t;

static uint32_t vector(vm86_t *vm, uint8_t n)
{
    uint8_t *m = vm86_memory(vm);
    return (uint32_t)m[n * 4] | (uint32_t)m[n * 4 + 1] << 8 | (uint32_t)m[n * 4 + 2] << 16 | (uint32_t)m[n * 4 + 3] << 24;
}

static void set_vector(vm86_t *vm, uint8_t n, uint32_t segoff)
{
    uint8_t *m = vm86_memory(vm);
    uint32_t lin = ((segoff >> 16) << 4) + (segoff & 0xFFFF);
    m[n * 4] = (uint8_t)segoff; m[n * 4 + 1] = (uint8_t)(segoff >> 8);
    m[n * 4 + 2] = (uint8_t)(segoff >> 16); m[n * 4 + 3] = (uint8_t)(segoff >> 24);
    m[lin] = 0xCF;   /* IRET in "ROM", in case a program calls the vector directly */
}

static uint8_t r_in8(void *u, uint16_t port)
{
    rig_t *r = (rig_t *)u;
    uint8_t v = 0xFF;
    int a = kbc_port_in(r->kbc, port, &v);
    if (a == KBC_PORT_NOT_MINE) a = pic_port_in(&r->pic, port, &v);
    if (a != 1) { r->bad_port = port; vm86_stop(r->cpu); }
    return v;
}

static void r_out8(void *u, uint16_t port, uint8_t value)
{
    rig_t *r = (rig_t *)u;
    int a = kbc_port_out(r->kbc, port, value);
    if (a == KBC_PORT_NOT_MINE) a = pic_port_out(&r->pic, port, value);
    if (a != 1) { r->bad_port = port; vm86_stop(r->cpu); }
}

static int r_int(void *u, vm86_t *vm, uint8_t n)
{
    rig_t *r = (rig_t *)u;
    vm86_regs_t *g = vm86_regs(vm);
    if (n == 0x09 && vector(vm, 0x09) == BIOS_INT09) {
        kbc_bios_irq(r->kbc);
        pic_eoi(&r->pic);
        return 1;
    }
    if (n == 0x16 && vector(vm, 0x16) == BIOS_INT16) {
        kbc_bios_regs_t b;
        int res;
        b.ax = g->ax; b.bx = g->bx; b.cx = g->cx; b.dx = g->dx; b.flags = g->flags;
        res = kbc_int16(r->kbc, &b);
        if (res == KBC_BIOS_WAIT) {
            g->ip -= 2;              /* run INT 16h again */
            g->flags |= VM86_IF;     /* the BIOS waits with interrupts enabled (STI) */
            return 1;
        }
        if (res == KBC_BIOS_UNSUPPORTED) { vm86_stop(vm); return 1; }
        g->ax = b.ax; g->flags = (uint16_t)((g->flags & ~0x40) | (b.flags & 0x40));
        return 1;
    }
    if (n == 0x20) { vm86_stop(vm); return 1; }
    return 0;
}

static void rig_init(rig_t *r, const char *src)
{
    asm86_result_t a;
    vm86_host_t h;
    memset(r, 0, sizeof(*r));
    if (asm86_assemble(src, &a) != 0) {
        printf("assembly error line %d: %s\n", a.errors[0].line, a.errors[0].message);
        exit(1);
    }
    r->cpu = vm86_create();
    r->kbd = kbd_create(NULL);
    r->kbc = kbc_create(vm86_memory(r->cpu));
    {
        kbc_device_t d = kbc_keyboard_device(r->kbd);
        kbc_connect(r->kbc, &d);
    }
    kbd_power(r->kbd, 1);
    kbc_reset(r->kbc);
    pic_reset(&r->pic);
    set_vector(r->cpu, 0x09, BIOS_INT09);
    set_vector(r->cpu, 0x16, BIOS_INT16);
    memset(&h, 0, sizeof(h));
    h.user = r; h.in8 = r_in8; h.out8 = r_out8; h.interrupt = r_int;
    vm86_set_host(r->cpu, &h);
    vm86_load_com(r->cpu, a.code, a.size, 0x1000);
    asm86_free(&a);
}

/* Runs ms milliseconds of emulated time in 40 us slices. */
static void rig_run(rig_t *r, int ms)
{
    int slices = ms * 25, i;
    for (i = 0; i < slices; i++) {
        vm86_status_t st = vm86_status(r->cpu);
        if (st != VM86_RUNNING && st != VM86_HALTED) return;
        vm86_run(r->cpu, SLICE);
        kbd_advance(r->kbd, 40);
        if (kbc_take_irq(r->kbc)) pic_raise(&r->pic, 1);
        if (vm86_regs(r->cpu)->flags & VM86_IF) {
            int v = pic_acknowledge(&r->pic);
            if (v >= 0) vm86_request_interrupt(r->cpu, (uint8_t)v);
        }
    }
}

static void type(rig_t *r, uint16_t key) { kbd_key(r->kbd, key, 1); rig_run(r, 30); kbd_key(r->kbd, key, 0); rig_run(r, 30); }

static void rig_done(rig_t *r) { kbc_destroy(r->kbc); kbd_destroy(r->kbd); vm86_destroy(r->cpu); }

static uint16_t word_at(rig_t *r, int sym_addr) { uint8_t *m = vm86_memory(r->cpu); return (uint16_t)(m[0x10000 + sym_addr] | m[0x10000 + sym_addr + 1] << 8); }

static int sym(const char *src, const char *name)
{
    asm86_result_t a;
    int i, v = -1;
    asm86_assemble(src, &a);
    for (i = 0; i < a.symbol_count; i++) if (!strcmp(a.symbols[i].name, name)) v = a.symbols[i].value;
    asm86_free(&a);
    return v;
}

int main(void)
{
    rig_t r;

    /* 1. Blocking reads through the BIOS (INT 16h AH=00h) */
    {
        const char *src =
            "org 100h\n"
            "    mov di, keys\n"
            "    mov cx, 5\n"
            "next: xor ah, ah\n"
            "    int 16h\n"
            "    mov [di], ax\n"
            "    add di, 2\n"
            "    loop next\n"
            "    int 20h\n"
            "keys: times 5 dw 0\n";
        int keys = sym(src, "keys");
        rig_init(&r, src);
        rig_run(&r, 50);
        CHECK(vm86_status(r.cpu) == VM86_RUNNING && word_at(&r, keys) == 0, "the program waits in INT 16h while no key is pressed");
        kbd_key(r.kbd, 0x2A, 1); rig_run(&r, 20);        /* Shift + h = 'H' */
        type(&r, 0x23);
        kbd_key(r.kbd, 0x2A, 0); rig_run(&r, 20);
        type(&r, 0x17);                                  /* i */
        type(&r, 0x1C);                                  /* Enter */
        type(&r, 0x3B);                                  /* F1 */
        type(&r, 0xE048);                                /* gray Up */
        rig_run(&r, 20);
        CHECK(word_at(&r, keys) == 0x2348 && word_at(&r, keys + 2) == 0x1769, "typed 'H' and 'i' arrive as 2348h, 1769h");
        CHECK(word_at(&r, keys + 4) == 0x1C0D && word_at(&r, keys + 6) == 0x3B00 && word_at(&r, keys + 8) == 0x4800,
              "Enter, F1 and gray Up arrive as 1C0Dh, 3B00h, 4800h");
        CHECK(vm86_status(r.cpu) == VM86_STOPPED, "the program finishes after 5 keys");
        rig_done(&r);
    }

    /* 2. Non-blocking polling (INT 16h AH=01h) in a busy loop, like a game */
    {
        const char *src =
            "org 100h\n"
            "poll: inc word [loops]\n"
            "    mov ah, 1\n"
            "    int 16h\n"
            "    jz poll\n"
            "    xor ah, ah\n"
            "    int 16h\n"
            "    mov [got], ax\n"
            "    cmp al, 27\n"
            "    jne poll\n"
            "    int 20h\n"
            "loops dw 0\n"
            "got dw 0\n";
        int got = sym(src, "got"), loops = sym(src, "loops");
        rig_init(&r, src);
        rig_run(&r, 30);
        type(&r, 0x1E);
        CHECK(word_at(&r, got) == 0x1E61 && word_at(&r, loops) > 100, "AH=01h polling sees the key while the program keeps running");
        type(&r, 0x01);
        CHECK(vm86_status(r.cpu) == VM86_STOPPED && word_at(&r, got) == 0x011B, "Esc ends the polling loop");
        rig_done(&r);
    }

    /* 3. A program with its own INT 09h handler reading port 60h (game style) */
    {
        const char *src =
            "org 100h\n"
            "    xor ax, ax\n"
            "    mov es, ax\n"
            "    cli\n"
            "    mov word [es:9*4], handler\n"
            "    mov [es:9*4+2], cs\n"
            "    sti\n"
            "wait_keys:\n"
            "    hlt\n"
            "    cmp word [count], 6\n"
            "    jb wait_keys\n"
            "    int 20h\n"
            "handler:\n"
            "    push ax\n"
            "    push bx\n"
            "    in al, 60h\n"
            "    mov bx, [cs:count]\n"
            "    mov [cs:codes+bx], al\n"
            "    inc word [cs:count]\n"
            "    mov al, 20h\n"
            "    out 20h, al\n"
            "    pop bx\n"
            "    pop ax\n"
            "    iret\n"
            "count dw 0\n"
            "codes times 8 db 0\n";
        int codes = sym(src, "codes");
        uint8_t *m;
        rig_init(&r, src);
        rig_run(&r, 20);
        CHECK(vm86_status(r.cpu) == VM86_HALTED, "the program sleeps in HLT waiting for IRQ 1");
        type(&r, 0xE048);    /* E0 48, E0 C8 */
        type(&r, 0x1E);      /* 1E, 9E */
        m = vm86_memory(r.cpu) + 0x10000 + codes;
        CHECK(m[0] == 0xE0 && m[1] == 0x48 && m[2] == 0xE0 && m[3] == 0xC8 && m[4] == 0x1E && m[5] == 0x9E,
              "its handler receives the raw scan codes E0 48 E0 C8 1E 9E");
        CHECK(vm86_status(r.cpu) == VM86_STOPPED, "HLT was woken up by each key interrupt");
        rig_done(&r);
    }

    /* 4. Keyboard commands from a program: set the LEDs */
    {
        rig_init(&r,
            "org 100h\n"
            "    mov al, 0EDh\n"
            "    out 60h, al\n"
            "    mov cx, 200\n"
            "d1: loop d1\n"
            "    mov al, 7\n"
            "    out 60h, al\n"
            "    mov cx, 200\n"
            "d2: loop d2\n"
            "    int 20h\n");
        rig_run(&r, 50);
        CHECK(kbd_leds(r.kbd) == 7 && vm86_status(r.cpu) == VM86_STOPPED, "OUT 60h EDh, 07h turns on the three LEDs (the BIOS eats the FAh replies)");
        rig_done(&r);
    }

    /* 5. Unsupported things are reported */
    {
        rig_init(&r, "org 100h\nmov al, 0D1h\nout 64h, al\nint 20h\n");
        rig_run(&r, 10);
        CHECK(r.bad_port == 0x64, "controller command D1h (A20 gate) is reported");
        rig_done(&r);
    }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
