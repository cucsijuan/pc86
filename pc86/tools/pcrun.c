/*
 * pcrun - runs a .COM program on a complete pc86 machine (VGA card, monitor,
 * keyboard) and saves what the monitor shows as PGM (mono) or PPM (color).
 *
 *   pcrun program.com [--seconds S] [--every N] [--out prefix] [--ips N] [--color]
 *
 * --every N saves a screenshot every N frames (60 frames per second).
 * A tiny DOS is provided for the program: INT 20h, INT 21h AH=02h, 09h, 4Ch.
 *
 * It is also the reference for what a game engine does: create the machine,
 * insert cards, plug peripherals, power on, then every frame pc_run() and
 * pc_vga_card_display().
 */
#include "pc.h"
#include "pc_vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dos {
    pc_vga_card_t *vga;
    int exit_code;
} dos_t;

static void teletype(dos_t *d, uint8_t ch)
{
    vga_bios_regs_t b;
    if (!pc_vga_card_vga(d->vga)) return;
    memset(&b, 0, sizeof(b));
    b.ax = (uint16_t)(0x0E00 | ch);
    b.bx = 0x0007;
    vga_int10(pc_vga_card_vga(d->vga), &b);
}

static int dos_int(void *user, pc_t *pc, uint8_t n)
{
    dos_t *d = (dos_t *)user;
    vm86_regs_t *r = vm86_regs(pc_cpu(pc));
    if (n == 0x20) { pc_stop(pc); return 1; }
    if (n != 0x21) return 0;
    switch (r->ax >> 8) {
    case 0x02: teletype(d, (uint8_t)r->dx); return 1;
    case 0x09: {
        uint8_t *m = pc_memory(pc);
        uint32_t a = ((uint32_t)r->ds << 4) + r->dx;
        int guard = 0;
        while (m[a & 0xFFFFF] != '$' && guard++ < 65536) teletype(d, m[a++ & 0xFFFFF]);
        return 1;
    }
    case 0x4C: d->exit_code = r->ax & 0xFF; pc_stop(pc); return 1;
    default: return 0;
    }
}

static int save(const char *prefix, const char *tag, const uint8_t *px, int channels)
{
    char name[512];
    FILE *f;
    snprintf(name, sizeof(name), "%s_%s.%s", prefix, tag, channels == 1 ? "pgm" : "ppm");
    f = fopen(name, "wb");
    if (!f) return -1;
    fprintf(f, "P%d\n640 400\n255\n", channels == 1 ? 5 : 6);
    fwrite(px, (size_t)channels, 640 * 400, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *prefix = "screen";
    double seconds = 5.0;
    int every = 0, use_color = 0, i, f, frames, channels;
    uint32_t ips = PC_DEFAULT_IPS;
    static uint8_t prog[0x10000], px[640 * 400 * 3];
    size_t n;
    FILE *fp;
    pc_t *pc;
    pc_vga_card_t *vga;
    monitor_t *mon;
    kbd_t *kbd;
    dos_t dos;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--ips") && i + 1 < argc) ips = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--color")) use_color = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: pcrun program.com [--seconds S] [--every N] [--out prefix] [--ips N] [--color]\n");
        return 2;
    }
    fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    n = fread(prog, 1, sizeof(prog), fp);
    fclose(fp);

    /* build the machine */
    pc = pc_create();
    vga = pc_vga_card_create();
    mon = monitor_create(monitor_builtin_model(use_color ? MONITOR_MODEL_VGA_COLOR : MONITOR_MODEL_VGA_MONO));
    kbd = kbd_create(NULL);
    if (!pc || !vga || !mon || !kbd) { fprintf(stderr, "out of memory\n"); return 2; }
    pc_insert_card(pc, 0, pc_vga_card_slot(vga));
    pc_vga_card_connect_monitor(vga, mon);
    pc_connect_keyboard(pc, kbd);
    memset(&dos, 0, sizeof(dos));
    dos.vga = vga;
    pc_set_int_hook(pc, dos_int, &dos);
    pc_set_speed(pc, ips);
    channels = monitor_channels(mon);

    pc_power_on(pc);
    pc_run(pc, 0.1);                       /* POST and boot message */
    if (pc_load_com(pc, prog, n) != 0) { fprintf(stderr, "program too large\n"); return 2; }

    frames = (int)(seconds * 60.0);
    for (f = 1; f <= frames; f++) {
        pc_status_t st;
        pc_run(pc, 1.0 / 60.0);
        pc_vga_card_display(vga, 1.0f / 60.0f, px);
        if (every > 0 && f % every == 0) {
            char tag[16];
            snprintf(tag, sizeof(tag), "%04d", f);
            save(prefix, tag, px, channels);
        }
        st = pc_status(pc);
        if (st != PC_RUNNING && st != PC_HALTED) break;
    }
    save(prefix, "final", px, channels);

    printf("emulated time: %.2f s, status %d\n", (double)pc_time_us(pc) / 1e6, (int)pc_status(pc));
    if (pc_error(pc)[0]) printf("error: %s\n", pc_error(pc));

    pc_destroy(pc);
    pc_vga_card_destroy(vga);
    monitor_destroy(mon);
    kbd_destroy(kbd);
    return pc_status(pc) == PC_ERROR ? 1 : dos.exit_code;
}
