/* Tests for the vm86 API features not covered by the instruction test suite. */
#include "asm86.h"
#include "vm86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static vm86_t *load(const char *src)
{
    asm86_result_t r;
    vm86_t *vm = vm86_create();
    if (asm86_assemble(src, &r) != 0) {
        printf("assembly error: %d: %s\n", r.errors[0].line, r.errors[0].message);
        exit(1);
    }
    vm86_load_com(vm, r.code, r.size, 0x1000);
    asm86_free(&r);
    return vm;
}

static int sym(const char *src, const char *name)
{
    asm86_result_t r;
    int k, v = -1;
    asm86_assemble(src, &r);
    for (k = 0; k < r.symbol_count; k++) if (!strcmp(r.symbols[k].name, name)) v = r.symbols[k].value;
    asm86_free(&r);
    return v;
}

static uint8_t last_out[0x10000];
static uint8_t in_value = 0;
static uint8_t t_in8(void *u, uint16_t p) { (void)u; (void)p; return in_value; }
static void t_out8(void *u, uint16_t p, uint8_t v) { (void)u; last_out[p] = v; }

int main(void)
{
    /* 1. A long REP STOSB is split across vm86_run calls */
    {
        vm86_t *vm = load("org 100h\nmov ax, 9000h\nmov es, ax\nxor di, di\nmov cx, 64000\nmov al, 7\nrep stosb\nhlt\n");
        uint32_t steps = vm86_run(vm, 5000);
        uint8_t *m = vm86_memory(vm);
        CHECK(steps == 5000 && vm86_status(vm) == VM86_RUNNING, "REP stops when the frame budget runs out");
        CHECK(vm86_regs(vm)->cx == 64000 - (5000 - 5), "CX reflects the iterations done so far");
        while (vm86_status(vm) == VM86_RUNNING) vm86_run(vm, 5000);
        CHECK(vm86_status(vm) == VM86_HALTED, "ends on HLT");
        CHECK(m[0x90000] == 7 && m[0x90000 + 63999] == 7 && m[0x90000 + 64000] == 0, "filled exactly 64000 bytes");
        vm86_destroy(vm);
    }

    /* 2. Save and load state: execution continues identically */
    {
        const char *src = "org 100h\nxor ax, ax\nmov cx, 1000\nnext:\nadd ax, cx\nrol ax, 1\nloop next\nhlt\n";
        vm86_t *a = load(src), *b = vm86_create();
        size_t n = vm86_state_size();
        void *buf = malloc(n);
        vm86_run(a, 700);
        CHECK(vm86_save_state(a, buf, n) == n, "save_state returns the full size");
        CHECK(vm86_load_state(b, buf, n) == 0, "load_state accepts the block");
        while (vm86_status(a) == VM86_RUNNING) vm86_run(a, 1000);
        while (vm86_status(b) == VM86_RUNNING) vm86_run(b, 1000);
        CHECK(memcmp(vm86_regs(a), vm86_regs(b), sizeof(vm86_regs_t)) == 0, "two machines from the same state end up identical");
        free(buf);
        vm86_destroy(a);
        vm86_destroy(b);
    }

    /* 3. HLT + hardware interrupt: wakes up, handles it and returns */
    {
        const char *src =
            "org 100h\n"
            "xor ax, ax\nmov es, ax\n"
            "mov word [es:8*4], tick\nmov [es:8*4+2], cs\n"
            "sti\n"
            "wait_loop: hlt\ncmp word [counter], 3\njb wait_loop\n"
            "done: hlt\n"
            "tick: inc word [cs:counter]\niret\n"
            "counter dw 0\n";
        vm86_t *vm = load(src);
        int i, counter = sym(src, "counter"), done = sym(src, "done");
        vm86_run(vm, 100);
        CHECK(vm86_status(vm) == VM86_HALTED, "the CPU waits in HLT");
        for (i = 0; i < 3; i++) {
            vm86_request_interrupt(vm, 8);
            vm86_run(vm, 100);
        }
        CHECK(vm86_memory(vm)[0x10000 + counter] == 3, "the INT 8 handler ran 3 times");
        CHECK(vm86_status(vm) == VM86_HALTED && vm86_regs(vm)->ip == done + 1, "after 3 interrupts it leaves the wait loop");
        vm86_destroy(vm);
    }

    /* 4. With IF=0 the interrupt stays queued */
    {
        vm86_t *vm = load("org 100h\ncli\nnop\nnop\nhlt\n");
        vm86_run(vm, 1);
        vm86_request_interrupt(vm, 8);
        vm86_run(vm, 2);
        CHECK(vm86_regs(vm)->ip == 0x103, "with IF=0 the interrupt is not serviced");
        vm86_destroy(vm);
    }

    /* 5. I/O ports */
    {
        vm86_t *vm = load("org 100h\nmov al, 0B6h\nout 43h, al\nmov dx, 3C8h\nmov al, 5\nout dx, al\nin al, 60h\nmov bl, al\nhlt\n");
        vm86_host_t h;
        memset(&h, 0, sizeof(h));
        h.in8 = t_in8;
        h.out8 = t_out8;
        vm86_set_host(vm, &h);
        in_value = 0x1E;
        vm86_run(vm, 100);
        CHECK(last_out[0x43] == 0xB6 && last_out[0x3C8] == 5, "OUT reaches the host (immediate port and DX)");
        CHECK((vm86_regs(vm)->bx & 0xFF) == 0x1E, "IN reads from the host");
        vm86_destroy(vm);
    }

    /* 6. Unsupported opcode: clear error, no side effects */
    {
        vm86_t *vm = load("org 100h\nmov ax, 1\ndb 27h\nmov ax, 2\n");
        vm86_error_info_t e;
        vm86_run(vm, 10);
        e = vm86_error(vm);
        CHECK(vm86_status(vm) == VM86_ERR_UNSUPPORTED, "DAA (27h) stops the CPU");
        CHECK(e.ip == 0x103 && e.opcode == 0x27 && vm86_regs(vm)->ip == 0x103 && vm86_regs(vm)->ax == 1,
              "the error reports address and opcode, and nothing else executes");
        vm86_destroy(vm);
    }

    /* 7. Direct far CALL, JMP and RETF: 1001:(x-10h) is the same byte as 1000:x */
    {
        const char *src =
            "org 100h\n"
            "call 1001h:(far_proc-10h)\n"
            "mov bx, cs\n"
            "jmp 0FFFh:(after+10h)\n"
            "after: mov dx, cs\n"
            "mov cx, 1111h\npush cx\npush cx\n"
            "call 1000h:proc4\n"
            "done: hlt\n"
            "far_proc: mov ax, cs\nretf\n"
            "proc4: mov si, sp\nretf 4\n";
        vm86_t *vm = load(src);
        vm86_regs_t *r = vm86_regs(vm);
        uint8_t *m = vm86_memory(vm);
        int done = sym(src, "done");
        vm86_run(vm, 100);
        CHECK(vm86_status(vm) == VM86_HALTED, "far calls and jumps run to the end");
        CHECK(r->ax == 0x1001 && r->bx == 0x1000, "CALL FAR (9A) loads CS and RETF (CB) restores it");
        CHECK(r->dx == 0x0FFF && r->cs == 0x0FFF && r->ip == done + 0x10 + 1, "JMP FAR (EA) loads CS:IP");
        CHECK(r->si == 0xFFFE - 8 && m[0x10000 + 0xFFFE - 8 + 2] == 0xFF && m[0x10000 + 0xFFFE - 8 + 3] == 0x0F,
              "CALL FAR pushes CS, then IP");
        CHECK(r->sp == 0xFFFE, "RETF 4 pops CS:IP and drops the 4 bytes of arguments");
        vm86_destroy(vm);
    }

    /* 8. Far CALL and JMP through a pointer in memory (FF /3, FF /5) */
    {
        const char *src =
            "org 100h\n"
            "mov si, jptr\n"
            "call far [cptr]\n"
            "mov cx, cs\n"
            "jmp far [si]\n"
            "hlt\n"
            "target: mov di, cs\nhlt\n"
            "cproc: mov bp, cs\nretf\n"
            "cptr: dw cproc-10h, 1001h\n"
            "jptr: dw target-20h, 1002h\n";
        vm86_t *vm = load(src);
        vm86_regs_t *r = vm86_regs(vm);
        int target = sym(src, "target");
        vm86_run(vm, 100);
        CHECK(vm86_status(vm) == VM86_HALTED && r->bp == 0x1001 && r->cx == 0x1000 && r->sp == 0xFFFE,
              "CALL FAR [mem] calls through the pointer and returns");
        CHECK(r->di == 0x1002 && r->cs == 0x1002 && r->ip == target - 0x20 + 3, "JMP FAR [mem] jumps through the pointer");
        vm86_destroy(vm);
    }

    /* 9. LDS and LES, including a far pointer from the vector table (INT 1Eh) */
    {
        const char *src =
            "org 100h\n"
            "les di, [ptr1]\n"
            "mov bx, ptr2\n"
            "lds si, [bx]\n"
            "xor ax, ax\nmov ds, ax\n"
            "lds bx, [1Eh*4]\n"
            "mov ax, cs\nmov ds, ax\n"
            "lds dx, [0FFFEh]\n"
            "hlt\n"
            "ptr1: dw 1234h, 0B800h\n"
            "ptr2: dw 5678h, 2000h\n";
        vm86_t *vm = load(src);
        vm86_regs_t *r = vm86_regs(vm);
        uint8_t *m = vm86_memory(vm);
        m[0x1E * 4] = 0xC7; m[0x1E * 4 + 1] = 0xEF; m[0x1E * 4 + 2] = 0x00; m[0x1E * 4 + 3] = 0xF0;
        m[0x1FFFE] = 0x34; m[0x1FFFF] = 0x12;   /* the segment word wraps to 1000:0000 (CD 20) */
        vm86_run(vm, 3);
        CHECK(r->di == 0x1234 && r->es == 0xB800, "LES loads the offset and ES");
        CHECK(r->si == 0x5678 && r->ds == 0x2000, "LDS loads the offset and DS");
        vm86_run(vm, 3);
        CHECK(r->bx == 0xEFC7 && r->ds == 0xF000, "LDS reads the INT 1Eh vector");
        vm86_run(vm, 100);
        CHECK(vm86_status(vm) == VM86_HALTED && r->dx == 0x1234 && r->ds == 0x20CD,
              "the pointer's segment word wraps around within the segment");
        vm86_destroy(vm);
    }

    /* 10. Register forms of LDS/LES and far CALL/JMP are undefined on the 8086 */
    {
        static const char *forms[] = {"db 0C5h, 0C6h", "db 0C4h, 0F8h", "db 0FFh, 0D8h", "db 0FFh, 0E8h"};
        int k, ok = 1;
        for (k = 0; k < 4; k++) {
            char src[64];
            vm86_t *vm;
            vm86_error_info_t e;
            sprintf(src, "org 100h\nmov ax, 1\n%s\nmov ax, 2\n", forms[k]);
            vm = load(src);
            vm86_run(vm, 10);
            e = vm86_error(vm);
            ok &= vm86_status(vm) == VM86_ERR_UNSUPPORTED && e.ip == 0x103 && vm86_regs(vm)->ip == 0x103 &&
                  vm86_regs(vm)->ax == 1 && vm86_regs(vm)->cs == 0x1000 && vm86_regs(vm)->sp == 0xFFFE;
            vm86_destroy(vm);
        }
        CHECK(ok, "they stop the CPU with no side effects");
    }

    /* 11. Assembler line map (for the debugger) */
    {
        asm86_result_t r;
        asm86_assemble("org 100h\n\n; comment\nmov ax, 1\nnop\ndata db 1,2,3\n", &r);
        CHECK(r.line_count == 3 && r.lines[0].line == 4 && r.lines[0].address == 0x100 && r.lines[0].size == 3 &&
              r.lines[1].line == 5 && r.lines[1].address == 0x103 && r.lines[2].size == 3,
              "lines -> addresses");
        asm86_free(&r);
    }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
