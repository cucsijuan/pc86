/*
 * vm86_glue.c - example of wiring the vga86 device to the vm86 CPU core.
 *
 * This file is also a small tool (vgarun) that runs a .COM program on
 * CPU + monitor and saves screenshots as PGM images:
 *
 *   vgarun program.com [--frames N] [--every K] [--out prefix] [--ips N]
 *
 * It is the reference for what the game engine does every frame:
 *   1. run the CPU in slices of about 40 us of emulated time, advancing the
 *      device clock by each slice's duration, so programs polling port 3DAh
 *      see the 63 us vertical retrace pulse (longer slices miss retraces);
 *   2. call vga_render() once per engine frame and upload the 640x400
 *      luminance buffer to a texture.
 *
 * Requires vm86 (the CPU core), which is a separate package:
 *   make demo VM86_DIR=../vm86
 */
#include "vga.h"
#include "vm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct machine {
    vm86_t *cpu;
    vga_t *vga;
    uint32_t ips;          /* emulated instructions per second */
    uint64_t ns_debt;      /* sub-microsecond remainder of the device clock */
    char error[128];       /* set when the program hits something unsupported */
    int exit_code;
} machine_t;

/* ---------- host callbacks for vm86 ---------- */

static uint8_t m_in8(void *user, uint16_t port)
{
    machine_t *m = (machine_t *)user;
    uint8_t value = 0xFF;
    int r = vga_port_in(m->vga, port, &value);
    if (r == VGA_PORT_UNSUPPORTED) {
        snprintf(m->error, sizeof(m->error), "unsupported video port read: IN %03Xh", port);
        vm86_stop(m->cpu);
    }
    /* VGA_PORT_NOT_MINE: other devices (keyboard, timer...) would go here */
    return value;
}

static void m_out8(void *user, uint16_t port, uint8_t value)
{
    machine_t *m = (machine_t *)user;
    if (vga_port_out(m->vga, port, value) == VGA_PORT_UNSUPPORTED) {
        snprintf(m->error, sizeof(m->error), "unsupported video port write: OUT %03Xh", port);
        vm86_stop(m->cpu);
    }
}

static void int10(machine_t *m, vm86_t *vm)
{
    vm86_regs_t *r = vm86_regs(vm);
    vga_bios_regs_t b;
    b.ax = r->ax; b.bx = r->bx; b.cx = r->cx; b.dx = r->dx; b.es = r->es; b.bp = r->bp;
    if (!vga_int10(m->vga, &b)) {
        snprintf(m->error, sizeof(m->error), "unsupported video BIOS call: INT 10h AX=%04Xh", r->ax);
        vm86_stop(vm);
        return;
    }
    r->ax = b.ax; r->bx = b.bx; r->cx = b.cx; r->dx = b.dx;
}

/* Minimal DOS so that text output shows up on the monitor. */
static void teletype(machine_t *m, uint8_t ch)
{
    vga_bios_regs_t b;
    memset(&b, 0, sizeof(b));
    b.ax = (uint16_t)(0x0E00 | ch);
    b.bx = 0x0007;
    vga_int10(m->vga, &b);
}

static int m_interrupt(void *user, vm86_t *vm, uint8_t n)
{
    machine_t *m = (machine_t *)user;
    vm86_regs_t *r = vm86_regs(vm);
    /* A full machine would only service these in C while the vector table
     * still points at its BIOS; that is part of the machine, not the monitor. */
    switch (n) {
    case 0x10:
        int10(m, vm);
        return 1;
    case 0x20:
        vm86_stop(vm);
        return 1;
    case 0x21:
        switch (r->ax >> 8) {
        case 0x02: teletype(m, (uint8_t)r->dx); return 1;
        case 0x09: {
            uint8_t *mem = vm86_memory(vm);
            uint32_t a = ((uint32_t)r->ds << 4) + r->dx;
            int guard = 0;
            while (mem[a & 0xFFFFF] != '$' && guard++ < 65536) teletype(m, mem[a++ & 0xFFFFF]);
            return 1;
        }
        case 0x4C:
            m->exit_code = r->ax & 0xFF;
            vm86_stop(vm);
            return 1;
        default:
            snprintf(m->error, sizeof(m->error), "INT 21h AH=%02Xh is not part of this demo", r->ax >> 8);
            vm86_stop(vm);
            return 1;
        }
    default:
        return 0;   /* real behavior: jump through the vector table */
    }
}

/* ---------- the per-frame loop ---------- */

#define SLICE_US 40   /* must stay below the 63 us retrace pulse */

/* Runs one engine frame of dt_seconds. Returns 0 when the program finished. */
static int machine_frame(machine_t *m, double dt_seconds)
{
    uint32_t budget = (uint32_t)(m->ips * dt_seconds);
    uint32_t slice = m->ips / (1000000u / SLICE_US);
    if (slice == 0) slice = 1;
    while (budget > 0 && vm86_status(m->cpu) == VM86_RUNNING) {
        uint32_t n = budget < slice ? budget : slice;
        uint32_t done = vm86_run(m->cpu, n);
        uint64_t ns;
        if (done == 0) done = n;                     /* halted: time still passes */
        ns = (uint64_t)done * 1000000000u / m->ips + m->ns_debt;
        vga_advance(m->vga, (uint32_t)(ns / 1000u));
        m->ns_debt = ns % 1000u;
        budget -= n;
    }
    if (vm86_status(m->cpu) == VM86_HALTED) {
        /* nothing wakes the CPU in this demo (no timer/keyboard devices yet) */
        vga_advance(m->vga, (uint32_t)(dt_seconds * 1e6));
    }
    return vm86_status(m->cpu) == VM86_RUNNING || vm86_status(m->cpu) == VM86_HALTED;
}

static int save_pgm(const char *path, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", VGA_OUT_WIDTH, VGA_OUT_HEIGHT);
    fwrite(px, 1, (size_t)VGA_OUT_WIDTH * VGA_OUT_HEIGHT, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *prefix = "screen";
    int frames = 120, every = 0, i, f;
    static uint8_t prog[0x10000], pixels[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];
    size_t n;
    FILE *fp;
    machine_t m;
    vm86_host_t host;
    char name[512];

    memset(&m, 0, sizeof(m));
    m.ips = 400000;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--ips") && i + 1 < argc) m.ips = (uint32_t)atol(argv[++i]);
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: vgarun program.com [--frames N] [--every K] [--out prefix] [--ips N]\n");
        return 2;
    }
    fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    n = fread(prog, 1, sizeof(prog), fp);
    fclose(fp);

    m.cpu = vm86_create();
    m.vga = vga_create(vm86_memory(m.cpu));
    memset(&host, 0, sizeof(host));
    host.user = &m;
    host.in8 = m_in8;
    host.out8 = m_out8;
    host.interrupt = m_interrupt;
    vm86_set_host(m.cpu, &host);
    vm86_load_com(m.cpu, prog, n, 0x1000);

    for (f = 1; f <= frames; f++) {
        int running = machine_frame(&m, 1.0 / 60.0);
        if (every > 0 && f % every == 0) {
            vga_render(m.vga, pixels);
            snprintf(name, sizeof(name), "%s_%04d.pgm", prefix, f);
            save_pgm(name, pixels);
        }
        if (!running) break;
    }
    vga_render(m.vga, pixels);
    snprintf(name, sizeof(name), "%s_final.pgm", prefix);
    save_pgm(name, pixels);

    printf("frames run: %d, device frames (70 Hz): %u, mode %02Xh, status %d\n",
           f > frames ? frames : f, vga_frame_count(m.vga), vga_mode(m.vga), (int)vm86_status(m.cpu));
    if (m.error[0]) printf("error: %s\n", m.error);
    if (vm86_status(m.cpu) == VM86_ERR_UNSUPPORTED) {
        vm86_error_info_t e = vm86_error(m.cpu);
        printf("error: unsupported instruction at %04X:%04X (opcode %02Xh)\n", e.cs, e.ip, e.opcode);
    }
    vga_destroy(m.vga);
    vm86_destroy(m.cpu);
    return m.error[0] ? 1 : m.exit_code;
}
