/*
 * pcrun - runs a complete pc86 machine (VGA card, monitor, keyboard and,
 * with --a/--b, the diskette adapter) and saves what the monitor shows as
 * PGM (mono) or PPM (color).
 *
 *   pcrun [program.com] [--a image] [--b image] [--a2 image] [--b2 image]
 *         [--protect a|b] [--save] [--type TEXT] [--until TEXT] [--text]
 *         [--seconds S] [--every N] [--out prefix] [--ips N] [--color]
 *
 * program.com is loaded after the POST, like DOS would. Without it the
 * machine boots from drive A: as usual.
 * --a / --b insert diskette images (.img, 1.44 MB or 720 KB) in the drives.
 * --a2 / --b2 are second diskettes, put in the drive instead of the first
 *   one when --type reaches \1 (A:) or \2 (B:).
 * --protect sets the write-protect tab of the diskette in that drive.
 * --save writes the images back when the machine changed them.
 * --type types TEXT on the keyboard, one key each time the BIOS keyboard
 *   buffer is empty. \n is Enter, \b Backspace, \e Escape, \t Tab, \c
 *   Ctrl+C, \\ a backslash; other control characters are Ctrl + a letter.
 *   \{Name} is any other key by its name (\{F2}, \{ArrowUp}, \{Delete}),
 *   with Ctrl+ or Shift+ in front if it is held (\{Ctrl+Home}).
 * --until stops once TEXT has been typed and TEXT is on the cursor's row.
 * --text prints the text screen at the end.
 * --every N saves a screenshot every N frames (60 frames per second).
 * A tiny DOS is provided for the program: INT 20h, INT 21h AH=02h, 09h, 4Ch
 * (only while nothing else has installed an INT 21h handler).
 *
 * It is also the reference for what a game engine does: create the machine,
 * insert cards, plug peripherals, power on, then every frame pc_run() and
 * pc_vga_card_display().
 */
#include "pc.h"
#include "pc_fdc.h"
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

/* ---------- diskettes ---------- */

static disk_t *load_image(const char *path)
{
    static uint8_t buf[2 * 1024 * 1024];
    size_t n;
    disk_t *d;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    d = disk_create_image(buf, n);
    if (!d) fprintf(stderr, "%s: not a 1.44 MB or 720 KB diskette image\n", path);
    return d;
}

static int save_image(const char *path, disk_t *d)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(disk_data(d), 1, disk_size(d), f) != disk_size(d)) {
        fprintf(stderr, "cannot write %s\n", path);
        if (f) fclose(f);
        return -1;
    }
    fclose(f);
    disk_clear_modified(d);
    return 0;
}

/* ---------- keyboard ---------- */

enum { MOD_NONE, MOD_SHIFT, MOD_CTRL };

/* Not keys: in the --type text, change the diskette in A: or B: */
#define SWAP_A 28
#define SWAP_B 29
/* In the --type text: a key by its name; the next byte is 1 + its place in named[] */
#define NAMED  30

static struct { uint16_t key; int mod; } named[255];
static int named_count;

/* US layout: the key for a character, and the modifier it needs. */
static uint16_t key_for(char c, int *shift)
{
    static const char *SHIFTED = "~!@#$%^&*()_+{}|:\"<>?";
    static const char *PLAIN = "`1234567890-=[]\\;',./";
    static const char *CODES[] = {
        "Backquote", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8",
        "Digit9", "Digit0", "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
        "Semicolon", "Quote", "Comma", "Period", "Slash"
    };
    char name[16];
    const char *p;
    *shift = 0;
    if (c >= 'a' && c <= 'z') { snprintf(name, sizeof(name), "Key%c", c - 'a' + 'A'); return kbd_key_from_code(name); }
    if (c >= 'A' && c <= 'Z') { *shift = MOD_SHIFT; snprintf(name, sizeof(name), "Key%c", c); return kbd_key_from_code(name); }
    if (c == ' ') return kbd_key_from_code("Space");
    if (c == '\n') return kbd_key_from_code("Enter");
    if (c == '\b') return kbd_key_from_code("Backspace");
    if (c == '\t') return kbd_key_from_code("Tab");
    if (c == 27) return kbd_key_from_code("Escape");
    if (c && (p = strchr(PLAIN, c)) != NULL) return kbd_key_from_code(CODES[p - PLAIN]);
    if (c && (p = strchr(SHIFTED, c)) != NULL) { *shift = MOD_SHIFT; return kbd_key_from_code(CODES[p - SHIFTED]); }
    if (c >= 1 && c <= 26) { *shift = MOD_CTRL; snprintf(name, sizeof(name), "Key%c", c - 1 + 'A'); return kbd_key_from_code(name); }
    return 0;
}

/* Turns the escapes of --type into the characters to type. */
static char *unescape(const char *s)
{
    char *out = (char *)malloc(strlen(s) + 1), *o = out;
    if (!out) return NULL;
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { *o++ = *s; continue; }
        s++;
        switch (*s) {
        case 'n': *o++ = '\n'; break;
        case 'b': *o++ = '\b'; break;
        case 't': *o++ = '\t'; break;
        case 'e': *o++ = 27; break;
        case 'c': *o++ = 3; break;
        case '1': *o++ = SWAP_A; break;
        case '2': *o++ = SWAP_B; break;
        case '{': {                    /* \{Ctrl+Name} */
            const char *end = strchr(s, '}');
            char name[32];
            int mod = MOD_NONE, n;
            uint16_t key;
            if (!end || end - s - 1 >= (int)sizeof(name)) { free(out); return NULL; }
            n = (int)(end - s - 1);
            memcpy(name, s + 1, (size_t)n);
            name[n] = 0;
            if (!strncmp(name, "Ctrl+", 5)) { mod = MOD_CTRL; memmove(name, name + 5, strlen(name + 5) + 1); }
            else if (!strncmp(name, "Shift+", 6)) { mod = MOD_SHIFT; memmove(name, name + 6, strlen(name + 6) + 1); }
            if (!(key = kbd_key_from_code(name))) { free(out); return NULL; }
            for (n = 0; n < named_count && (named[n].key != key || named[n].mod != mod); n++) {}
            if (n == named_count) {
                if (n == 255) { free(out); return NULL; }
                named[n].key = key;
                named[n].mod = mod;
                named_count++;
            }
            *o++ = NAMED;
            *o++ = (char)(n + 1);
            s = end;
            break;
        }
        default:  *o++ = *s; break;
        }
    }
    *o = 0;
    return out;
}

/* ---------- text screen ---------- */

static const uint8_t *text_row(pc_t *pc, int row) { return pc_memory(pc) + 0xB8000 + row * 160; }

static int cursor_row_has(pc_t *pc, const char *s)
{
    const uint8_t *m = pc_memory(pc);
    int row = m[0x451], col, len = (int)strlen(s), k;
    if (row > 24) return 0;
    for (col = 0; col + len <= 80; col++) {
        for (k = 0; k < len && text_row(pc, row)[(col + k) * 2] == (uint8_t)s[k]; k++) {}
        if (k == len) return 1;
    }
    return 0;
}

static void print_text(pc_t *pc)
{
    int row, col, last = -1;
    for (row = 0; row < 25; row++)
        for (col = 0; col < 80; col++)
            if (text_row(pc, row)[col * 2] > ' ') last = row;
    for (row = 0; row <= last; row++) {
        char line[81];
        int n = 0;
        for (col = 0; col < 80; col++) {
            uint8_t c = text_row(pc, row)[col * 2];
            line[col] = (char)(c < ' ' ? ' ' : c);
            if (c > ' ') n = col + 1;
        }
        line[n] = 0;
        printf("%s\n", line);
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL, *prefix = "screen", *img_path[4] = {NULL, NULL, NULL, NULL}, *until = NULL;
    char *keys = NULL;
    double seconds = 5.0;
    int every = 0, use_color = 0, save_disks = 0, show_text = 0, i, f, frames, channels, ret;
    int typed = 0, held = 0, shift = 0, protect[2] = {0, 0};
    uint16_t held_key = 0;
    uint32_t ips = PC_DEFAULT_IPS;
    static uint8_t prog[0x10000], px[640 * 400 * 3];
    size_t n = 0;
    FILE *fp;
    pc_t *pc;
    pc_vga_card_t *vga;
    pc_fdc_card_t *fdc = NULL;
    fdd_t *drive[2] = {NULL, NULL};
    disk_t *disk[4] = {NULL, NULL, NULL, NULL};    /* A:, B:, the second A:, the second B: */
    monitor_t *mon;
    kbd_t *kbd;
    dos_t dos;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--ips") && i + 1 < argc) ips = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--color")) use_color = 1;
        else if (!strcmp(argv[i], "--a") && i + 1 < argc) img_path[0] = argv[++i];
        else if (!strcmp(argv[i], "--b") && i + 1 < argc) img_path[1] = argv[++i];
        else if (!strcmp(argv[i], "--a2") && i + 1 < argc) img_path[2] = argv[++i];
        else if (!strcmp(argv[i], "--b2") && i + 1 < argc) img_path[3] = argv[++i];
        else if (!strcmp(argv[i], "--protect") && i + 1 < argc) {
            i++;
            if (argv[i][0] == 'a' || argv[i][0] == 'A') protect[0] = 1;
            else if (argv[i][0] == 'b' || argv[i][0] == 'B') protect[1] = 1;
        }
        else if (!strcmp(argv[i], "--save")) save_disks = 1;
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
            if (!(keys = unescape(argv[++i]))) { fprintf(stderr, "--type: unknown key in \\{...}\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) until = argv[++i];
        else if (!strcmp(argv[i], "--text")) show_text = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { path = NULL; img_path[0] = img_path[1] = NULL; break; }
    }
    if (!path && !img_path[0] && !img_path[1]) {
        fprintf(stderr, "usage: pcrun [program.com] [--a image] [--b image] [--a2 image] [--b2 image]\n"
                        "             [--protect a|b] [--save] [--type TEXT] [--until TEXT] [--text]\n"
                        "             [--seconds S] [--every N] [--out prefix] [--ips N] [--color]\n");
        return 2;
    }
    if (path) {
        fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 2; }
        n = fread(prog, 1, sizeof(prog), fp);
        fclose(fp);
    }

    /* build the machine */
    pc = pc_create();
    vga = pc_vga_card_create();
    mon = monitor_create(monitor_builtin_model(use_color ? MONITOR_MODEL_VGA_COLOR : MONITOR_MODEL_VGA_MONO));
    kbd = kbd_create(NULL);
    if (!pc || !vga || !mon || !kbd) { fprintf(stderr, "out of memory\n"); return 2; }
    pc_insert_card(pc, 0, pc_vga_card_slot(vga));
    pc_vga_card_connect_monitor(vga, mon);
    pc_connect_keyboard(pc, kbd);
    if (img_path[0] || img_path[1]) {
        fdc = pc_fdc_card_create();
        if (!fdc) { fprintf(stderr, "out of memory\n"); return 2; }
        pc_insert_card(pc, 1, pc_fdc_card_slot(fdc));
        for (i = 0; i < 2; i++) {
            drive[i] = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
            pc_fdc_card_connect_drive(fdc, i, drive[i]);
            if (img_path[i]) {
                if (!(disk[i] = load_image(img_path[i]))) return 2;
                disk_set_write_protect(disk[i], protect[i]);
                fdd_insert(drive[i], disk[i]);
            }
            if (img_path[i + 2] && !(disk[i + 2] = load_image(img_path[i + 2]))) return 2;
        }
    }
    memset(&dos, 0, sizeof(dos));
    dos.vga = vga;
    pc_set_int_hook(pc, dos_int, &dos);
    pc_set_speed(pc, ips);
    channels = monitor_channels(mon);

    pc_power_on(pc);
    if (path) {
        pc_run(pc, 0.1);                   /* POST and boot message */
        if (pc_load_com(pc, prog, n) != 0) { fprintf(stderr, "program too large\n"); return 2; }
    }

    frames = (int)(seconds * 60.0);
    for (f = 1; f <= frames; f++) {
        pc_status_t st;
        const uint8_t *m = pc_memory(pc);
        if (held) {                        /* release the key typed last frame */
            kbd_key(kbd, held_key, 0);
            if (shift) kbd_key(kbd, kbd_key_from_code(shift == MOD_CTRL ? "ControlLeft" : "ShiftLeft"), 0);
            held = 0;
        } else if (keys && (keys[typed] == SWAP_A || keys[typed] == SWAP_B)) {
            int d = keys[typed++] - SWAP_A;
            if (drive[d]) {
                fdd_eject(drive[d]);
                if (disk[d + 2]) fdd_insert(drive[d], disk[d + 2]);
            }
        } else if (keys && keys[typed] && f > 6 && m[0x41A] == m[0x41C]) {
            if (keys[typed] == NAMED) {
                int k = (uint8_t)keys[++typed] - 1;
                held_key = named[k].key;
                shift = named[k].mod;
            } else {
                held_key = key_for(keys[typed], &shift);
                if (!held_key) { fprintf(stderr, "cannot type character %02Xh\n", (uint8_t)keys[typed]); return 2; }
            }
            if (shift) kbd_key(kbd, kbd_key_from_code(shift == MOD_CTRL ? "ControlLeft" : "ShiftLeft"), 1);
            kbd_key(kbd, held_key, 1);
            held = 1;
            typed++;
        }
        pc_run(pc, 1.0 / 60.0);
        pc_vga_card_display(vga, 1.0f / 60.0f, px);
        if (every > 0 && f % every == 0) {
            char tag[16];
            snprintf(tag, sizeof(tag), "%04d", f);
            save(prefix, tag, px, channels);
        }
        st = pc_status(pc);
        if (st != PC_RUNNING && st != PC_HALTED) break;
        if (until && !held && (!keys || !keys[typed]) && pc_memory(pc)[0x41A] == pc_memory(pc)[0x41C] &&
            cursor_row_has(pc, until)) break;
    }
    save(prefix, "final", px, channels);

    if (show_text) print_text(pc);
    printf("emulated time: %.2f s, status %d\n", (double)pc_time_us(pc) / 1e6, (int)pc_status(pc));
    if (pc_error(pc)[0]) printf("error: %s\n", pc_error(pc));
    ret = pc_status(pc) == PC_ERROR ? 1 : dos.exit_code;
    if (until && f > frames) { printf("timeout: \"%s\" not reached\n", until); ret = 3; }
    for (i = 0; i < 4; i++)
        if (disk[i] && save_disks && disk_modified(disk[i]) && save_image(img_path[i], disk[i])) ret = 2;

    pc_destroy(pc);
    for (i = 0; i < 2; i++)
        if (drive[i]) { fdd_eject(drive[i]); fdd_destroy(drive[i]); }
    for (i = 0; i < 4; i++)
        if (disk[i]) disk_destroy(disk[i]);
    if (fdc) pc_fdc_card_destroy(fdc);
    pc_vga_card_destroy(vga);
    monitor_destroy(mon);
    kbd_destroy(kbd);
    free(keys);
    return ret;
}
