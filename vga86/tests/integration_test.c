/*
 * Integration tests with the vm86 CPU core and assembler (separate package).
 * Build with: make test-integration VM86_DIR=../vm86
 *
 * Programs talk to a "marker" port E9h: OUT E9h, AL records the current
 * device frame count so the test can measure timing as the program sees it.
 */
#include "asm86.h"
#include "vga.h"
#include "vm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

typedef struct {
    vm86_t *cpu;
    vga_t *vga;
    uint32_t marks[16];
    int nmarks;
    int bad_port;
    int bad_int10;
} rig_t;

static uint8_t r_in8(void *u, uint16_t port)
{
    rig_t *r = (rig_t *)u;
    uint8_t v = 0xFF;
    if (vga_port_in(r->vga, port, &v) == VGA_PORT_UNSUPPORTED) { r->bad_port = port; vm86_stop(r->cpu); }
    return v;
}

static void r_out8(void *u, uint16_t port, uint8_t value)
{
    rig_t *r = (rig_t *)u;
    if (port == 0xE9) {
        if (r->nmarks < 16) r->marks[r->nmarks++] = vga_frame_count(r->vga);
        return;
    }
    if (vga_port_out(r->vga, port, value) == VGA_PORT_UNSUPPORTED) { r->bad_port = port; vm86_stop(r->cpu); }
}

static int r_int(void *u, vm86_t *vm, uint8_t n)
{
    rig_t *r = (rig_t *)u;
    vm86_regs_t *g = vm86_regs(vm);
    if (n == 0x10) {
        vga_bios_regs_t b;
        b.ax = g->ax; b.bx = g->bx; b.cx = g->cx; b.dx = g->dx; b.es = g->es; b.bp = g->bp;
        if (!vga_int10(r->vga, &b)) { r->bad_int10 = g->ax; vm86_stop(vm); return 1; }
        g->ax = b.ax; g->bx = b.bx; g->cx = b.cx; g->dx = b.dx;
        return 1;
    }
    if (n == 0x20) { vm86_stop(vm); return 1; }
    return 0;
}

/* Runs a program to completion, advancing the device clock per slice. */
static void run(rig_t *r, const char *src, uint32_t ips, uint32_t slice)
{
    asm86_result_t a;
    vm86_host_t h;
    uint64_t ns_debt = 0;
    uint32_t total = 0;

    memset(r, 0, sizeof(*r));
    if (asm86_assemble(src, &a) != 0) {
        printf("assembly error line %d: %s\n", a.errors[0].line, a.errors[0].message);
        exit(1);
    }
    r->cpu = vm86_create();
    r->vga = vga_create(vm86_memory(r->cpu));
    memset(&h, 0, sizeof(h));
    h.user = r; h.in8 = r_in8; h.out8 = r_out8; h.interrupt = r_int;
    vm86_set_host(r->cpu, &h);
    vm86_load_com(r->cpu, a.code, a.size, 0x1000);
    asm86_free(&a);

    while (vm86_status(r->cpu) == VM86_RUNNING && total < 50000000u) {
        uint32_t done = vm86_run(r->cpu, slice);
        uint64_t ns = (uint64_t)done * 1000000000u / ips + ns_debt;
        vga_advance(r->vga, (uint32_t)(ns / 1000u));
        ns_debt = ns % 1000u;
        total += done;
    }
}

static void done(rig_t *r)
{
    vga_destroy(r->vga);
    vm86_destroy(r->cpu);
}

static const char *RETRACE_70 =
    "org 100h\n"
    "    call wait_retrace\n"
    "    out 0E9h, al\n"            /* mark A */
    "    mov cx, 70\n"
    "again:\n"
    "    call wait_retrace\n"
    "    loop again\n"
    "    out 0E9h, al\n"            /* mark B */
    "    int 20h\n"
    "wait_retrace:\n"
    "    mov dx, 3DAh\n"
    ".in:  in al, dx\n"
    "    test al, 8\n"
    "    jnz .in\n"
    ".out: in al, dx\n"
    "    test al, 8\n"
    "    jz .out\n"
    "    ret\n";

int main(void)
{
    rig_t r;
    static const uint32_t speeds[] = {100000, 300000, 400000, 1000000, 4770000};
    int i;

    /* 1. Polling 3DAh never misses a retrace with 40 us slices, at any speed */
    for (i = 0; i < 5; i++) {
        char msg[128];
        run(&r, RETRACE_70, speeds[i], speeds[i] / 25000 ? speeds[i] / 25000 : 1);
        snprintf(msg, sizeof(msg), "70 retrace waits take exactly 70 frames at %u instructions/s", speeds[i]);
        CHECK(r.nmarks == 2 && r.marks[1] - r.marks[0] == 70, msg);
        done(&r);
    }

    /* 2. Mode set, palette ports and drawing from a real program */
    run(&r,
        "org 100h\n"
        "    mov ax, 13h\n"
        "    int 10h\n"
        "    mov dx, 3C8h\n"
        "    mov al, 7\n"
        "    out dx, al\n"
        "    inc dx\n"
        "    mov al, 63\n"
        "    out dx, al\n"
        "    out dx, al\n"
        "    out dx, al\n"
        "    mov ax, 0A000h\n"
        "    mov es, ax\n"
        "    xor di, di\n"
        "    mov al, 7\n"
        "    mov cx, 320\n"
        "    rep stosb\n"
        "    int 20h\n", 400000, 16);
    {
        static uint8_t px[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];
        vga_render(r.vga, px);
        CHECK(vga_mode(r.vga) == 0x13 && px[0] == 255 && px[639] == 255 && px[VGA_OUT_WIDTH] == 255 &&
              px[2 * VGA_OUT_WIDTH] == 0, "program sets mode 13h, programs the DAC and draws the top row white");
    }
    done(&r);

    /* 3. Text written through DOS-style teletype ends up on screen */
    run(&r,
        "org 100h\n"
        "    mov si, msg\n"
        "next: lodsb\n"
        "    test al, al\n"
        "    jz fin\n"
        "    mov ah, 0Eh\n"
        "    int 10h\n"
        "    jmp next\n"
        "fin: int 20h\n"
        "msg db 'OK', 0\n", 400000, 16);
    {
        uint8_t *m = vm86_memory(r.cpu);
        CHECK(m[0xB8000] == 'O' && m[0xB8002] == 'K', "INT 10h teletype from a program writes text memory");
    }
    done(&r);

    /* 4. Unsupported hardware stops the program with a clear reason */
    run(&r, "org 100h\nmov dx, 3C4h\nmov al, 2\nout dx, al\nint 20h\n", 400000, 16);
    CHECK(r.bad_port == 0x3C4 && vm86_status(r.cpu) == VM86_STOPPED, "OUT to the sequencer (3C4h) is reported");
    done(&r);
    run(&r, "org 100h\nmov ax, 12h\nint 10h\nint 20h\n", 400000, 16);
    CHECK(r.bad_int10 == 0x0012 && vm86_status(r.cpu) == VM86_STOPPED, "INT 10h mode 12h is reported");
    done(&r);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
