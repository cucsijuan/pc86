/*
 * fdc86 - FAT12 on a diskette. See include/fat12.h.
 */
#include "fat12.h"
#include "fdc_internal.h"

#include <string.h>

typedef struct bpb {
    unsigned bytes, spc, reserved, fats, root_entries, total, spf;
    unsigned root_start, root_sectors, data_start, clusters;
} bpb_t;

static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int read_bpb(const disk_t *d, bpb_t *b)
{
    const uint8_t *s = d->data;
    unsigned total_sectors = (unsigned)(d->geo->size / SECTOR_SIZE);
    b->bytes = rd16(s + 0x0B);
    b->spc = s[0x0D];
    b->reserved = rd16(s + 0x0E);
    b->fats = s[0x10];
    b->root_entries = rd16(s + 0x11);
    b->total = rd16(s + 0x13);
    b->spf = rd16(s + 0x16);
    if (b->bytes != SECTOR_SIZE || !b->spc || (b->spc & (b->spc - 1)) || !b->reserved || !b->fats || b->fats > 2 ||
        !b->root_entries || b->root_entries % 16 || !b->spf || b->total > total_sectors || s[0x15] < 0xF0)
        return -1;
    b->root_start = b->reserved + b->fats * b->spf;
    b->root_sectors = b->root_entries * 32 / SECTOR_SIZE;
    b->data_start = b->root_start + b->root_sectors;
    if (b->data_start >= b->total) return -1;
    b->clusters = (b->total - b->data_start) / b->spc;
    return b->clusters >= 4085 ? -1 : 0;                        /* more would be FAT16 */
}

static uint8_t *sector(const disk_t *d, unsigned n) { return d->data + (size_t)n * SECTOR_SIZE; }

static unsigned fat_get(const disk_t *d, const bpb_t *b, unsigned n)
{
    const uint8_t *fat = sector(d, b->reserved);
    unsigned off = n * 3 / 2, v = rd16(fat + off);
    return n & 1 ? v >> 4 : v & 0xFFF;
}

static void fat_set(disk_t *d, const bpb_t *b, unsigned n, unsigned v)
{
    unsigned i, off = n * 3 / 2;
    for (i = 0; i < b->fats; i++) {
        uint8_t *fat = sector(d, b->reserved + i * b->spf);
        unsigned w = rd16(fat + off);
        w = n & 1 ? (w & 0x000F) | (v << 4) : (w & 0xF000) | (v & 0xFFF);
        wr16(fat + off, w);
    }
}

/* "name.ext" -> "NAME    EXT". -1 if it is not a valid 8.3 name. */
static int to83(const char *name, char out[11])
{
    int i = 0, n = 0;
    const char *bad = "\"*+,/:;<=>?[\\]|";
    memset(out, ' ', 11);
    for (; *name && *name != '.'; name++, n++) {
        if (n >= 8 || *name == ' ') return -1;
        out[n] = *name;
    }
    if (*name == '.') {
        for (name++; *name; name++, i++) {
            if (i >= 3 || *name == ' ') return -1;
            out[8 + i] = *name;
        }
    }
    if (out[0] == ' ') return -1;
    for (i = 0; i < 11; i++) {
        unsigned char ch = (unsigned char)out[i];
        if (ch >= 'a' && ch <= 'z') out[i] = (char)(ch - 32);
        else if (ch < 0x20 || ch == '.' || strchr(bad, ch)) return -1;
    }
    return 0;
}

static void from83(const uint8_t *e, char out[13])
{
    int i, n = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
}

static uint8_t *dir_entry(const disk_t *d, const bpb_t *b, unsigned i)
{
    return sector(d, b->root_start) + (size_t)i * 32;
}

static uint8_t *find(const disk_t *d, const bpb_t *b, const char *name)
{
    char n83[11];
    unsigned i;
    if (to83(name, n83)) return NULL;
    for (i = 0; i < b->root_entries; i++) {
        uint8_t *e = dir_entry(d, b, i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || (e[11] & 0x08)) continue;           /* deleted, label */
        if (!memcmp(e, n83, 11)) return e;
    }
    return NULL;
}

/* ---------- format and boot sector ---------- */

/* Boot code at 7C3Eh: prints the message, waits for a key, INT 19h. */
static const uint8_t NOT_BOOTABLE[] = {
    0xFA,                         /* cli */
    0x31, 0xC0,                   /* xor ax, ax */
    0x8E, 0xD8,                   /* mov ds, ax */
    0x8E, 0xD0,                   /* mov ss, ax */
    0xBC, 0x00, 0x7C,             /* mov sp, 7C00h */
    0xFB,                         /* sti */
    0xBE, 0x60, 0x7C,             /* mov si, msg */
    0xAC,                         /* next: lodsb */
    0x08, 0xC0,                   /* or al, al */
    0x74, 0x09,                   /* jz wait */
    0xB4, 0x0E,                   /* mov ah, 0Eh */
    0xBB, 0x07, 0x00,             /* mov bx, 7 */
    0xCD, 0x10,                   /* int 10h */
    0xEB, 0xF2,                   /* jmp next */
    0x31, 0xC0,                   /* wait: xor ax, ax */
    0xCD, 0x16,                   /* int 16h */
    0xCD, 0x19,                   /* int 19h */
};
static const char NOT_BOOTABLE_MSG[] = "\r\nNon-system disk\r\nPress any key to restart\r\n";

int fat12_format(disk_t *d, const char *label)
{
    const disk_geometry_t *g = d->geo;
    uint8_t *s = d->data;
    int hd = g->sectors == 18;
    unsigned spc = hd ? 1 : 2, root = hd ? 224 : 112, spf = hd ? 9 : 3, i;
    char lab[11];
    bpb_t b;

    memset(d->data, 0xF6, d->geo->size);
    memset(s, 0, SECTOR_SIZE);
    s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;                      /* jmp short 3Eh / nop */
    memcpy(s + 3, "PC86    ", 8);
    wr16(s + 0x0B, SECTOR_SIZE);
    s[0x0D] = (uint8_t)spc;
    wr16(s + 0x0E, 1);                                          /* reserved: the boot sector */
    s[0x10] = 2;
    wr16(s + 0x11, root);
    wr16(s + 0x13, g->size / SECTOR_SIZE);
    s[0x15] = hd ? 0xF0 : 0xF9;                                 /* media descriptor */
    wr16(s + 0x16, spf);
    wr16(s + 0x18, g->sectors);
    wr16(s + 0x1A, g->heads);
    s[0x24] = 0x00;                                             /* drive number */
    s[0x26] = 0x29;                                             /* extended boot signature */
    s[0x27] = 0x86; s[0x28] = 0x19; s[0x29] = 0x20; s[0x2A] = 0x26;   /* serial number */
    memset(lab, ' ', 11);
    if (label) for (i = 0; i < 11 && label[i]; i++) lab[i] = (char)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);
    memcpy(s + 0x2B, label ? lab : "NO NAME    ", 11);
    memcpy(s + 0x36, "FAT12   ", 8);
    memcpy(s + 0x3E, NOT_BOOTABLE, sizeof(NOT_BOOTABLE));
    memcpy(s + 0x60, NOT_BOOTABLE_MSG, sizeof(NOT_BOOTABLE_MSG));
    s[510] = 0x55; s[511] = 0xAA;

    read_bpb(d, &b);
    for (i = 0; i < 2; i++) memset(sector(d, b.reserved + i * spf), 0, (size_t)spf * SECTOR_SIZE);
    memset(sector(d, b.root_start), 0, (size_t)b.root_sectors * SECTOR_SIZE);
    fat_set(d, &b, 0, 0xF00u | s[0x15]);
    fat_set(d, &b, 1, 0xFFF);
    if (label) {
        uint8_t *e = dir_entry(d, &b, 0);
        memcpy(e, lab, 11);
        e[11] = 0x08;
        wr16(e + 24, 0x0021);                                   /* 1 Jan 1980 */
    }
    d->modified = 1;
    return 0;
}

int fat12_valid(const disk_t *d)
{
    bpb_t b;
    return read_bpb(d, &b) == 0;
}

int fat12_install_boot(disk_t *d, const uint8_t *code, size_t len)
{
    uint8_t keep[0x3B];
    bpb_t b;
    if (len > 510 || read_bpb(d, &b)) return -1;
    memcpy(keep, d->data + 3, sizeof(keep));
    memset(d->data, 0, 510);
    memcpy(d->data, code, len);
    memcpy(d->data + 3, keep, sizeof(keep));
    d->data[510] = 0x55; d->data[511] = 0xAA;
    d->modified = 1;
    return 0;
}

/* ---------- files ---------- */

static void free_chain(disk_t *d, const bpb_t *b, unsigned cl)
{
    unsigned guard = 0;
    while (cl >= 2 && cl < b->clusters + 2 && guard++ < b->clusters) {
        unsigned next = fat_get(d, b, cl);
        fat_set(d, b, cl, 0);
        cl = next;
    }
}

int fat12_delete_file(disk_t *d, const char *name)
{
    bpb_t b;
    uint8_t *e;
    if (read_bpb(d, &b) || !(e = find(d, &b, name)) || (e[11] & 0x10)) return -1;
    free_chain(d, &b, rd16(e + 26));
    e[0] = 0xE5;
    d->modified = 1;
    return 0;
}

long fat12_free_bytes(const disk_t *d)
{
    bpb_t b;
    unsigned i, n = 0;
    if (read_bpb(d, &b)) return -1;
    for (i = 2; i < b.clusters + 2; i++) n += fat_get(d, &b, i) == 0;
    return (long)n * (long)(b.spc * SECTOR_SIZE);
}

int fat12_write_file(disk_t *d, const char *name, const uint8_t *data, size_t len)
{
    bpb_t b;
    char n83[11];
    unsigned i, need, prev = 0, first = 0, cl, csize;
    uint8_t *e = NULL, *old;
    long avail;

    if (read_bpb(d, &b) || to83(name, n83)) return -1;
    old = find(d, &b, name);
    if (old && (old[11] & 0x19)) return -1;                     /* directory, label, read-only */
    csize = b.spc * SECTOR_SIZE;
    need = (unsigned)((len + csize - 1) / csize);
    avail = fat12_free_bytes(d) + (old ? (long)((rd16(old + 28) | (unsigned long)rd16(old + 30) << 16) + csize - 1) / (long)csize * (long)csize : 0);
    if ((long)need * (long)csize > avail) return -1;
    if (old) { free_chain(d, &b, rd16(old + 26)); e = old; }
    else {
        for (i = 0; i < b.root_entries; i++) {
            uint8_t *x = dir_entry(d, &b, i);
            if (x[0] == 0x00 || x[0] == 0xE5) { e = x; break; }
        }
        if (!e) return -1;                                      /* root directory full */
    }
    for (cl = 2, i = 0; i < need && cl < b.clusters + 2; cl++) {
        size_t off = (size_t)i * csize, n = len - off < csize ? len - off : csize;
        if (fat_get(d, &b, cl)) continue;
        memset(sector(d, b.data_start + (cl - 2) * b.spc), 0, csize);
        memcpy(sector(d, b.data_start + (cl - 2) * b.spc), data + off, n);
        if (prev) fat_set(d, &b, prev, cl); else first = cl;
        fat_set(d, &b, cl, 0xFFF);
        prev = cl;
        i++;
    }
    memset(e, 0, 32);
    memcpy(e, n83, 11);
    e[11] = 0x20;                                               /* archive */
    wr16(e + 24, 0x0021);
    wr16(e + 26, first);
    wr16(e + 28, (unsigned)(len & 0xFFFF));
    wr16(e + 30, (unsigned)(len >> 16));
    d->modified = 1;
    return 0;
}

long fat12_file_size(const disk_t *d, const char *name)
{
    bpb_t b;
    const uint8_t *e;
    if (read_bpb(d, &b) || !(e = find(d, &b, name)) || (e[11] & 0x10)) return -1;
    return (long)(rd16(e + 28) | (unsigned long)rd16(e + 30) << 16);
}

long fat12_read_file(const disk_t *d, const char *name, uint8_t *buf, size_t cap)
{
    bpb_t b;
    const uint8_t *e;
    unsigned long size, done = 0;
    unsigned cl, csize, guard = 0;
    if (read_bpb(d, &b) || !(e = find(d, &b, name)) || (e[11] & 0x10)) return -1;
    size = rd16(e + 28) | (unsigned long)rd16(e + 30) << 16;
    if (size > cap) size = (unsigned long)cap;
    csize = b.spc * SECTOR_SIZE;
    cl = rd16(e + 26);
    while (done < size && cl >= 2 && cl < b.clusters + 2 && guard++ < b.clusters) {
        unsigned long n = size - done < csize ? size - done : csize;
        memcpy(buf + done, sector(d, b.data_start + (cl - 2) * b.spc), n);
        done += n;
        cl = fat_get(d, &b, cl);
    }
    return (long)done;
}

int fat12_list(const disk_t *d, fat12_entry_t *out, int max)
{
    bpb_t b;
    unsigned i;
    int n = 0;
    if (read_bpb(d, &b)) return -1;
    for (i = 0; i < b.root_entries && n < max; i++) {
        const uint8_t *e = dir_entry(d, &b, i);
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F) continue;            /* deleted, long name */
        if (e[11] & 0x08) {                                     /* volume label: 11 characters */
            int k;
            memcpy(out[n].name, e, 11);
            for (k = 11; k > 0 && out[n].name[k - 1] == ' '; k--) {}
            out[n].name[k] = 0;
        } else from83(e, out[n].name);
        out[n].attr = e[11];
        out[n].size = (uint32_t)(rd16(e + 28) | (unsigned long)rd16(e + 30) << 16);
        n++;
    }
    return n;
}
