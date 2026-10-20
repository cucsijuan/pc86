/*
 * fdimage - makes and edits diskette images (.img) for the machine.
 *
 *   fdimage new  disk.img [--720k] [--label NAME]   formatted FAT12 diskette
 *   fdimage ls   disk.img                           root directory
 *   fdimage add  disk.img file [NAME.EXT]           copy a file in (replaces)
 *   fdimage get  disk.img NAME.EXT [file]           copy a file out
 *   fdimage rm   disk.img NAME.EXT
 *   fdimage boot disk.img boot.bin                  install a boot sector, keeping the BPB
 */
#include "fat12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!data || (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n)) { fclose(f); free(data); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return data;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f) fclose(f);
    return ok ? 0 : -1;
}

static disk_t *open_image(const char *path)
{
    size_t len;
    uint8_t *data = read_file(path, &len);
    disk_t *d;
    if (!data) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    d = disk_create_image(data, len);
    free(data);
    if (!d) fprintf(stderr, "%s: not a 1.44 MB or 720 KB image (%lu bytes)\n", path, (unsigned long)len);
    else if (!fat12_valid(d)) { fprintf(stderr, "%s: no FAT12 file system\n", path); disk_destroy(d); d = NULL; }
    return d;
}

static int save_image(disk_t *d, const char *path)
{
    if (write_file(path, disk_data(d), disk_size(d)) != 0) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    return 0;
}

/* The last path component, which is also the default 8.3 name. */
static const char *base_name(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: fdimage new  disk.img [--720k] [--label NAME]\n"
            "       fdimage ls   disk.img\n"
            "       fdimage add  disk.img file [NAME.EXT]\n"
            "       fdimage get  disk.img NAME.EXT [file]\n"
            "       fdimage rm   disk.img NAME.EXT\n"
            "       fdimage boot disk.img boot.bin\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *cmd, *img;
    disk_t *d;
    int rc = 0, i;

    if (argc < 3) return usage();
    cmd = argv[1];
    img = argv[2];

    if (!strcmp(cmd, "new")) {
        disk_format_t format = DISK_1440K;
        const char *label = NULL;
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--720k")) format = DISK_720K;
            else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
            else return usage();
        }
        d = disk_create(format);
        fat12_format(d, label);
        rc = save_image(d, img);
        disk_destroy(d);
        return rc;
    }

    if (!(d = open_image(img))) return 1;
    if (!strcmp(cmd, "ls") && argc == 3) {
        fat12_entry_t e[224];
        int n = fat12_list(d, e, 224);
        for (i = 0; i < n; i++) {
            if (e[i].attr & 0x08) printf("  volume %s\n", e[i].name);
            else if (e[i].attr & 0x10) printf("  %-12s  <DIR>\n", e[i].name);
            else printf("  %-12s %8lu\n", e[i].name, (unsigned long)e[i].size);
        }
        printf("  %ld bytes free\n", fat12_free_bytes(d));
    } else if (!strcmp(cmd, "add") && (argc == 4 || argc == 5)) {
        size_t len;
        uint8_t *data = read_file(argv[3], &len);
        const char *name = argc == 5 ? argv[4] : base_name(argv[3]);
        if (!data) { fprintf(stderr, "cannot read %s\n", argv[3]); rc = 1; }
        else if (fat12_write_file(d, name, data, len) != 0) {
            fprintf(stderr, "cannot add %s: not an 8.3 name, read-only, or no space\n", name);
            rc = 1;
        } else rc = save_image(d, img);
        free(data);
    } else if (!strcmp(cmd, "get") && (argc == 4 || argc == 5)) {
        long size = fat12_file_size(d, argv[3]);
        uint8_t *data;
        if (size < 0) { fprintf(stderr, "%s: no such file\n", argv[3]); rc = 1; }
        else {
            data = (uint8_t *)malloc(size ? (size_t)size : 1);
            fat12_read_file(d, argv[3], data, (size_t)size);
            rc = write_file(argc == 5 ? argv[4] : argv[3], data, (size_t)size) ? 1 : 0;
            if (rc) fprintf(stderr, "cannot write %s\n", argc == 5 ? argv[4] : argv[3]);
            free(data);
        }
    } else if (!strcmp(cmd, "rm") && argc == 4) {
        if (fat12_delete_file(d, argv[3]) != 0) { fprintf(stderr, "%s: no such file\n", argv[3]); rc = 1; }
        else rc = save_image(d, img);
    } else if (!strcmp(cmd, "boot") && argc == 4) {
        size_t len;
        uint8_t *data = read_file(argv[3], &len);
        if (!data) { fprintf(stderr, "cannot read %s\n", argv[3]); rc = 1; }
        else if (fat12_install_boot(d, data, len > 510 && len == 512 ? 510 : len) != 0) {
            fprintf(stderr, "%s: a boot sector has room for 510 bytes (%lu)\n", argv[3], (unsigned long)len);
            rc = 1;
        } else rc = save_image(d, img);
        free(data);
    } else rc = usage();
    disk_destroy(d);
    return rc;
}
