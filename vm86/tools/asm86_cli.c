/*
 * asm86 - command-line assembler (cross-development tool).
 *
 *   asm86 program.asm [-o program.com] [-l listing.txt]
 *
 * Errors are printed as "file:line: error: message", like NASM, so editors
 * recognize them.
 */
#include "asm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

static void write_listing(FILE *f, const char *src, const asm86_result_t *r)
{
    const char *p = src;
    int line = 1, k = 0;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        fprintf(f, "%5d ", line);
        if (k < r->line_count && r->lines[k].line == line) {
            const asm86_line_t *L = &r->lines[k];
            int i, shown = L->size > 8 ? 8 : L->size;
            size_t off = (size_t)(L->address - r->origin);
            fprintf(f, "%04X ", L->address);
            for (i = 0; i < 8; i++) {
                if (i < shown && off + (size_t)i < r->size) fprintf(f, "%02X", r->code[off + (size_t)i]);
                else fprintf(f, "  ");
            }
            fprintf(f, "%c ", L->size > 8 ? '+' : ' ');
            k++;
        } else {
            fprintf(f, "%23s", "");
        }
        fprintf(f, "%.*s\n", (int)(e - p), p);
        p = *e ? e + 1 : e;
        line++;
    }
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *lst = NULL;
    char outbuf[1024];
    char *src;
    asm86_result_t r;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) lst = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        else in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: asm86 program.asm [-o program.com] [-l listing.txt]\n");
        return 2;
    }
    if (!out) {
        const char *dot = strrchr(in, '.');
        size_t n = dot ? (size_t)(dot - in) : strlen(in);
        if (n > sizeof(outbuf) - 5) n = sizeof(outbuf) - 5;
        memcpy(outbuf, in, n);
        strcpy(outbuf + n, ".com");
        out = outbuf;
    }
    src = read_file(in);
    if (!src) { fprintf(stderr, "cannot read %s\n", in); return 2; }

    if (asm86_assemble(src, &r) != 0) {
        for (i = 0; i < r.error_count; i++)
            fprintf(stderr, "%s:%d: error: %s\n", in, r.errors[i].line, r.errors[i].message);
        asm86_free(&r);
        free(src);
        return 1;
    }
    {
        FILE *f = fopen(out, "wb");
        if (!f || (r.size && fwrite(r.code, 1, r.size, f) != r.size)) { fprintf(stderr, "cannot write %s\n", out); return 2; }
        fclose(f);
    }
    if (lst) {
        FILE *f = fopen(lst, "w");
        if (f) { write_listing(f, src, &r); fclose(f); }
    }
    asm86_free(&r);
    free(src);
    return 0;
}
