/*
 * run86 - runs a .COM file on the vm86 core with a minimal test "mini-DOS".
 * This is NOT the game machine (that is step 3); it is for testing programs
 * from the terminal.
 *
 *   run86 program.com [--max N] [--regs]
 *
 * Services: INT 20h (exit), INT 21h AH=01 (read key with echo),
 * AH=02 (write DL), AH=08 (read key), AH=09 (write '$'-terminated string at
 * DS:DX), AH=4Ch (exit with code AL).
 */
#include "vm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exit_code = 0;

static int dos(void *user, vm86_t *vm, uint8_t n)
{
    vm86_regs_t *r = vm86_regs(vm);
    uint8_t *mem = vm86_memory(vm);
    (void)user;
    if (n == 0x20) { vm86_stop(vm); return 1; }
    if (n != 0x21) return 0;
    switch (r->ax >> 8) {
    case 0x01: case 0x08: {
        int c = getchar();
        if (c == EOF) c = 0;
        if ((r->ax >> 8) == 0x01) putchar(c);
        r->ax = (uint16_t)((r->ax & 0xFF00) | (c & 0xFF));
        return 1;
    }
    case 0x02: putchar(r->dx & 0xFF); return 1;
    case 0x09: {
        uint16_t off = r->dx;
        int guard = 0;
        while (guard++ < 65536) {
            uint8_t c = mem[(((uint32_t)r->ds << 4) + off) & 0xFFFFF];
            if (c == '$') break;
            putchar(c);
            off++;
        }
        return 1;
    }
    case 0x4C: exit_code = r->ax & 0xFF; vm86_stop(vm); return 1;
    default:
        fprintf(stderr, "\n[run86] INT 21h AH=%02Xh is not implemented in the mini-DOS\n", r->ax >> 8);
        vm86_stop(vm);
        exit_code = 255;
        return 1;
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned long max = 100000000UL;
    int show_regs = 0, i;
    static uint8_t buf[0x10000];
    size_t n;
    FILE *f;
    vm86_t *vm;
    vm86_host_t host;
    unsigned long total = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max") && i + 1 < argc) max = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--regs")) show_regs = 1;
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: run86 program.com [--max N] [--regs]\n"); return 2; }
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    vm = vm86_create();
    memset(&host, 0, sizeof(host));
    host.interrupt = dos;
    vm86_set_host(vm, &host);
    if (vm86_load_com(vm, buf, n, 0x1000) != 0) { fprintf(stderr, "program too large\n"); return 2; }

    while (total < max && vm86_status(vm) == VM86_RUNNING)
        total += vm86_run(vm, 100000);
    fflush(stdout);

    switch (vm86_status(vm)) {
    case VM86_STOPPED: break;
    case VM86_HALTED: fprintf(stderr, "\n[run86] the CPU executed HLT\n"); break;
    case VM86_ERR_UNSUPPORTED: {
        vm86_error_info_t e = vm86_error(vm);
        fprintf(stderr, "\n[run86] unsupported instruction at %04X:%04X (opcode %02Xh", e.cs, e.ip, e.opcode);
        if (e.modrm_reg != 0xFF) fprintf(stderr, " /%d", e.modrm_reg);
        fprintf(stderr, ")\n");
        exit_code = 254;
        break;
    }
    default: fprintf(stderr, "\n[run86] step limit of %lu reached\n", max); exit_code = 253; break;
    }
    if (show_regs) {
        vm86_regs_t *r = vm86_regs(vm);
        fprintf(stderr, "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n"
                        "CS=%04X DS=%04X ES=%04X SS=%04X IP=%04X FLAGS=%04X  steps=%lu\n",
                r->ax, r->bx, r->cx, r->dx, r->si, r->di, r->bp, r->sp,
                r->cs, r->ds, r->es, r->ss, r->ip, r->flags, total);
    }
    vm86_destroy(vm);
    return exit_code;
}
