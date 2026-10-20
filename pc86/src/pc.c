/*
 * pc86 - the motherboard. See include/pc.h.
 */
#include "pc.h"

#include "asm86.h"
#include "kbc.h"
#include "pic.h"
#include "pit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System BIOS ROM layout (F000:xxxx), at the offsets the IBM BIOS uses */
#define ROM_SEG        0xF000u
#define ROM_BASE       0xF0000u
#define OFF_POST       0xE05Bu   /* boot: INT 19h, then idle in HLT */
#define OFF_INT16      0xE82Eu
#define OFF_INT19      0xE6F2u
#define OFF_INT09      0xE987u
#define OFF_INT12      0xF841u
#define OFF_INT11      0xF84Du
#define OFF_INT1A      0xFE6Eu
#define OFF_INT08      0xFEA5u   /* timer tick: real 8086 code */
#define OFF_IRQ_OTHER  0xFF23u   /* unexpected hardware interrupt: EOI + IRET */
#define OFF_DUMMY      0xFF53u   /* IRET */
#define OFF_RESET      0xFFF0u

/* Option ROM entry points of the card in each slot */
#define CARD_ROM_SEG(slot) (0xC000u + (uint32_t)(slot) * 0x80u)

#define BDA            0x400u
#define BDA_EQUIPMENT  0x410u
#define BDA_MEMORY_KB  0x413u
#define BDA_KBD_FLAGS3 0x496u
#define BDA_TICKS      0x46Cu
#define BDA_MIDNIGHT   0x470u

#define SLICE_NS ((uint64_t)PC_SLICE_US * 1000u)

typedef struct pc_board {
    uint8_t powered;
    uint8_t status;               /* pc_status_t */
    uint8_t port61;               /* bits 0-3 as written */
    uint64_t time_us;
    uint64_t ns_carry;            /* part of a slice not run yet */
    uint64_t step_carry;          /* fraction of an instruction, in 1/1000000 */
} pc_board_t;

struct pc {
    vm86_t *cpu;
    pic_t pic;
    pit_t *pit;
    kbc_t *kbc;
    kbd_t *kbd;
    pc_card_t *slot[PC_SLOTS];
    pc_board_t b;
    uint32_t ips;
    pc_int_hook_t hook;
    void *hook_user;
    char error[160];
    uint8_t rom[0x10000];
};

/* ---------- helpers ---------- */

static uint8_t *mem(pc_t *pc) { return vm86_memory(pc->cpu); }

static uint16_t rd16(pc_t *pc, uint32_t a) { uint8_t *m = mem(pc); return (uint16_t)(m[a] | m[a + 1] << 8); }
static void wr16(pc_t *pc, uint32_t a, uint16_t v) { uint8_t *m = mem(pc); m[a] = (uint8_t)v; m[a + 1] = (uint8_t)(v >> 8); }

static uint32_t vector(pc_t *pc, uint8_t n) { return (uint32_t)rd16(pc, n * 4u + 2) << 16 | rd16(pc, n * 4u); }
static uint32_t far_ptr(uint32_t seg, uint32_t off) { return seg << 16 | off; }

static void set_vector(pc_t *pc, uint8_t n, uint32_t seg, uint32_t off)
{
    wr16(pc, n * 4u, (uint16_t)off);
    wr16(pc, n * 4u + 2, (uint16_t)seg);
}

static void fail(pc_t *pc, const char *msg)
{
    if (pc->b.status == PC_ERROR) return;
    snprintf(pc->error, sizeof(pc->error), "%s", msg);
    pc->b.status = PC_ERROR;
    vm86_stop(pc->cpu);
}

/* ---------- the system BIOS ROM ---------- */

static const char *SRC_POST =
    "org 0E05Bh\n"
    "    sti\n"
    "    int 19h\n"              /* bootstrap; returns when there is nothing to boot */
    "idle:\n"
    "    hlt\n"
    "    jmp idle\n";

static const char *SRC_INT08 =
    "org 0FEA5h\n"
    "    sti\n"
    "    push ds\n"
    "    push ax\n"
    "    mov ax, 40h\n"
    "    mov ds, ax\n"
    "    inc word [6Ch]\n"       /* timer ticks since midnight */
    "    jnz count_ok\n"
    "    inc word [6Eh]\n"
    "count_ok:\n"
    "    cmp word [6Eh], 18h\n"  /* 1800B0h ticks = 24 hours */
    "    jne no_midnight\n"
    "    cmp word [6Ch], 0B0h\n"
    "    jne no_midnight\n"
    "    mov word [6Eh], 0\n"
    "    mov word [6Ch], 0\n"
    "    mov byte [70h], 1\n"
    "no_midnight:\n"
    "    int 1Ch\n"              /* user timer hook */
    "    mov al, 20h\n"
    "    out 20h, al\n"
    "    pop ax\n"
    "    pop ds\n"
    "    iret\n";

static const char *SRC_IRQ_OTHER =
    "org 0FF23h\n"
    "    push ax\n"
    "    mov al, 20h\n"
    "    out 20h, al\n"
    "    pop ax\n"
    "    iret\n";

static int place(pc_t *pc, const char *src)
{
    asm86_result_t a;
    int ok = asm86_assemble(src, &a) == 0 && a.error_count == 0 && (uint32_t)a.origin + a.size <= 0x10000u;
    if (ok) memcpy(pc->rom + a.origin, a.code, a.size);
    asm86_free(&a);
    return ok ? 0 : -1;
}

static int build_rom(pc_t *pc)
{
    static const uint16_t c_entries[] = {OFF_INT09, OFF_INT11, OFF_INT12, OFF_INT16, OFF_INT19, OFF_INT1A, OFF_DUMMY};
    static const uint8_t reset[] = {0xEA, 0x5B, 0xE0, 0x00, 0xF0};      /* JMP F000:E05B */
    static const char date[] = "10/20/26";
    size_t i;
    memset(pc->rom, 0xFF, sizeof(pc->rom));
    for (i = 0; i < sizeof(c_entries) / sizeof(c_entries[0]); i++) pc->rom[c_entries[i]] = 0xCF;   /* IRET */
    if (place(pc, SRC_POST) || place(pc, SRC_INT08) || place(pc, SRC_IRQ_OTHER)) return -1;
    memcpy(pc->rom + OFF_RESET, reset, sizeof(reset));
    memcpy(pc->rom + 0xFFF5, date, 8);
    pc->rom[0xFFFE] = 0xFE;                                          /* model byte: PC/XT class */
    return 0;
}

/* ---------- BIOS services in C ---------- */

static int card_bios(pc_t *pc, uint8_t n, vm86_regs_t *r, int *handled);

static void bios_teletype(pc_t *pc, const char *s)
{
    vm86_regs_t *r = vm86_regs(pc->cpu), save = *r;
    int handled;
    for (; *s; s++) {
        r->ax = (uint16_t)(0x0E00 | (uint8_t)*s);
        r->bx = 0x0007;
        card_bios(pc, 0x10, r, &handled);
        if (!handled) break;                                         /* no video card */
    }
    *r = save;
}

static int int16(pc_t *pc, vm86_regs_t *g)
{
    kbc_bios_regs_t b;
    int res;
    b.ax = g->ax; b.bx = g->bx; b.cx = g->cx; b.dx = g->dx; b.flags = g->flags;
    res = kbc_int16(pc->kbc, &b);
    if (res == KBC_BIOS_WAIT) {
        g->ip -= 2;                                                  /* run INT 16h again */
        g->flags |= VM86_IF;                                         /* the BIOS waits with STI */
        return 1;
    }
    if (res == KBC_BIOS_UNSUPPORTED) {
        char msg[96];
        snprintf(msg, sizeof(msg), "unsupported keyboard BIOS call: INT 16h AX=%04Xh", g->ax);
        fail(pc, msg);
        return 1;
    }
    g->ax = b.ax;
    g->flags = (uint16_t)((g->flags & ~0x40) | (b.flags & 0x40));
    return 1;
}

static int int1a(pc_t *pc, vm86_regs_t *g)
{
    uint8_t *m = mem(pc);
    switch (g->ax >> 8) {
    case 0x00:
        g->cx = rd16(pc, BDA_TICKS + 2);
        g->dx = rd16(pc, BDA_TICKS);
        g->ax = (uint16_t)((g->ax & 0xFF00) | m[BDA_MIDNIGHT]);
        m[BDA_MIDNIGHT] = 0;
        return 1;
    case 0x01:
        wr16(pc, BDA_TICKS + 2, g->cx);
        wr16(pc, BDA_TICKS, g->dx);
        m[BDA_MIDNIGHT] = 0;
        return 1;
    default: {
        char msg[96];
        snprintf(msg, sizeof(msg), "unsupported time BIOS call: INT 1Ah AX=%04Xh (no real-time clock)", g->ax);
        fail(pc, msg);
        return 1;
    }
    }
}

/* INT n whose vector points at the option ROM of a card. */
static int card_bios(pc_t *pc, uint8_t n, vm86_regs_t *r, int *handled)
{
    uint32_t v = vector(pc, n);
    int s, j;
    *handled = 0;
    for (s = 0; s < PC_SLOTS; s++) {
        pc_card_t *c = pc->slot[s];
        if (!c || !c->bios) continue;
        for (j = 0; j < c->rom_int_count && j < 4; j++) {
            if (c->rom_ints[j] != n || v != far_ptr(CARD_ROM_SEG(s), (uint32_t)j * 4)) continue;
            *handled = 1;
            return c->bios(c->user, n, r);
        }
    }
    return PC_BIOS_UNSUPPORTED;
}

/* ---------- CPU callbacks ---------- */

static uint8_t port61_read(pc_t *pc)
{
    return (uint8_t)((pc->b.port61 & 0x0Fu) | ((pit_edges(pc->pit, 1) & 1u) << 4) | (pit_output(pc->pit, 2) ? 0x20u : 0u));
}

static uint8_t m_in8(void *user, uint16_t port)
{
    pc_t *pc = (pc_t *)user;
    uint8_t v = 0xFF;
    int r = pic_port_in(&pc->pic, port, &v), s;
    if (!r) r = pit_port_in(pc->pit, port, &v);
    if (!r) r = kbc_port_in(pc->kbc, port, &v);
    if (!r && port == 0x61) { v = port61_read(pc); r = 1; }
    for (s = 0; !r && s < PC_SLOTS; s++)
        if (pc->slot[s] && pc->slot[s]->port_in) r = pc->slot[s]->port_in(pc->slot[s]->user, port, &v);
    if (r < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported port read: IN %03Xh", port);
        fail(pc, msg);
    }
    return r == 1 ? v : 0xFF;                                        /* nobody there: open bus */
}

static void m_out8(void *user, uint16_t port, uint8_t value)
{
    pc_t *pc = (pc_t *)user;
    int r = pic_port_out(&pc->pic, port, value), s;
    if (!r) r = pit_port_out(pc->pit, port, value);
    if (!r) r = kbc_port_out(pc->kbc, port, value);
    if (!r && port == 0x61) {
        pc->b.port61 = (uint8_t)(value & 0x0F);
        pit_set_gate(pc->pit, 2, value & 1);
        r = 1;
    }
    for (s = 0; !r && s < PC_SLOTS; s++)
        if (pc->slot[s] && pc->slot[s]->port_out) r = pc->slot[s]->port_out(pc->slot[s]->user, port, value);
    if (r < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported port write: OUT %03Xh, %02Xh", port, value);
        fail(pc, msg);
    }
}

static int m_interrupt(void *user, vm86_t *vm, uint8_t n)
{
    pc_t *pc = (pc_t *)user;
    vm86_regs_t *g = vm86_regs(vm);
    uint32_t v = vector(pc, n);
    int handled, res;

    if (v >> 16 == ROM_SEG) {
        uint32_t off = v & 0xFFFF;
        if (n == 0x09 && off == OFF_INT09) { kbc_bios_irq(pc->kbc); pic_eoi(&pc->pic); return 1; }
        if (n == 0x16 && off == OFF_INT16) return int16(pc, g);
        if (n == 0x11 && off == OFF_INT11) { g->ax = rd16(pc, BDA_EQUIPMENT); return 1; }
        if (n == 0x12 && off == OFF_INT12) { g->ax = rd16(pc, BDA_MEMORY_KB); return 1; }
        if (n == 0x1A && off == OFF_INT1A) return int1a(pc, g);
        if (n == 0x19 && off == OFF_INT19) {
            bios_teletype(pc, "No boot device available.\r\n");    /* no storage yet */
            return 1;
        }
    }
    res = card_bios(pc, n, g, &handled);
    if (handled) {
        if (res == PC_BIOS_WAIT) { g->ip -= 2; g->flags |= VM86_IF; }
        else if (res == PC_BIOS_UNSUPPORTED) {
            char msg[96];
            snprintf(msg, sizeof(msg), "unsupported BIOS call: INT %02Xh AX=%04Xh", n, g->ax);
            fail(pc, msg);
        }
        return 1;
    }
    if (pc->hook && (v == 0 || v == far_ptr(ROM_SEG, OFF_DUMMY)) && pc->hook(pc->hook_user, pc, n)) return 1;
    return 0;                                                        /* through the vector */
}

/* ---------- machine ---------- */

pc_t *pc_create(void)
{
    pc_t *pc = (pc_t *)calloc(1, sizeof(pc_t));
    vm86_host_t h;
    if (!pc) return NULL;
    pc->cpu = vm86_create();
    pc->pit = pit_create();
    pc->kbc = pc->cpu ? kbc_create(vm86_memory(pc->cpu)) : NULL;
    if (!pc->cpu || !pc->pit || !pc->kbc || build_rom(pc) != 0) { pc_destroy(pc); return NULL; }
    pc->ips = PC_DEFAULT_IPS;
    memset(&h, 0, sizeof(h));
    h.user = pc;
    h.in8 = m_in8;
    h.out8 = m_out8;
    h.interrupt = m_interrupt;
    vm86_set_host(pc->cpu, &h);
    pic_reset(&pc->pic);
    return pc;
}

void pc_destroy(pc_t *pc)
{
    if (!pc) return;
    if (pc->kbc) kbc_destroy(pc->kbc);
    if (pc->pit) pit_destroy(pc->pit);
    if (pc->cpu) vm86_destroy(pc->cpu);
    free(pc);
}

vm86_t *pc_cpu(pc_t *pc) { return pc->cpu; }
uint8_t *pc_memory(pc_t *pc) { return mem(pc); }

int pc_insert_card(pc_t *pc, int slot, pc_card_t *card)
{
    if (pc->b.powered || slot < 0 || slot >= PC_SLOTS || pc->slot[slot] || !card) return -1;
    pc->slot[slot] = card;
    if (card->attach) card->attach(card->user, mem(pc));
    return 0;
}

pc_card_t *pc_remove_card(pc_t *pc, int slot)
{
    pc_card_t *c;
    if (pc->b.powered || slot < 0 || slot >= PC_SLOTS) return NULL;
    c = pc->slot[slot];
    pc->slot[slot] = NULL;
    return c;
}

pc_card_t *pc_card(pc_t *pc, int slot) { return slot >= 0 && slot < PC_SLOTS ? pc->slot[slot] : NULL; }

static void update_keyboard_type(pc_t *pc)
{
    uint8_t *m = mem(pc);
    int enhanced = pc->kbd && (kbd_model(pc->kbd)->keys & KBD_KEYS_ENHANCED);
    if (enhanced) m[BDA_KBD_FLAGS3] |= 0x10; else m[BDA_KBD_FLAGS3] &= (uint8_t)~0x10;
}

void pc_connect_keyboard(pc_t *pc, kbd_t *keyboard)
{
    if (pc->kbd) {
        kbd_power(pc->kbd, 0);
        kbc_connect(pc->kbc, NULL);
    }
    pc->kbd = keyboard;
    if (keyboard) {
        kbc_device_t d = kbc_keyboard_device(keyboard);
        kbc_connect(pc->kbc, &d);
        if (pc->b.powered) kbd_power(keyboard, 1);                  /* self test, AAh */
    }
}

kbd_t *pc_keyboard(pc_t *pc) { return pc->kbd; }

static void post(pc_t *pc)
{
    uint8_t *m = mem(pc);
    vm86_regs_t *r;
    int i, s, j;

    memset(m, 0, VM86_MEM_SIZE);
    memcpy(m + ROM_BASE, pc->rom, sizeof(pc->rom));
    vm86_reset(pc->cpu);

    /* interrupt vectors */
    for (i = 0; i < 0x20; i++) set_vector(pc, (uint8_t)i, ROM_SEG, OFF_DUMMY);
    for (i = 0x0A; i <= 0x0F; i++) set_vector(pc, (uint8_t)i, ROM_SEG, OFF_IRQ_OTHER);
    set_vector(pc, 0x08, ROM_SEG, OFF_INT08);
    set_vector(pc, 0x09, ROM_SEG, OFF_INT09);
    set_vector(pc, 0x11, ROM_SEG, OFF_INT11);
    set_vector(pc, 0x12, ROM_SEG, OFF_INT12);
    set_vector(pc, 0x16, ROM_SEG, OFF_INT16);
    set_vector(pc, 0x19, ROM_SEG, OFF_INT19);
    set_vector(pc, 0x1A, ROM_SEG, OFF_INT1A);

    /* BIOS data area */
    wr16(pc, BDA_EQUIPMENT, 0x0000);   /* no floppies, no coprocessor, EGA/VGA-class video, no ports */
    wr16(pc, BDA_MEMORY_KB, 640);

    /* board chips */
    pic_reset(&pc->pic);
    pit_reset(pc->pit);
    pc->b.port61 = 0;
    pit_set_gate(pc->pit, 2, 0);

    /* keyboard: power cycle it, as on a real power-up, then the controller's POST */
    if (pc->kbd) { kbd_power(pc->kbd, 0); kbd_power(pc->kbd, 1); }
    kbc_reset(pc->kbc);
    update_keyboard_type(pc);

    /* cards: power on, then hook their option ROM services */
    for (s = 0; s < PC_SLOTS; s++) {
        pc_card_t *c = pc->slot[s];
        if (!c) continue;
        if (c->power) c->power(c->user, 1);
        for (j = 0; j < c->rom_int_count && j < 4; j++) {
            set_vector(pc, c->rom_ints[j], CARD_ROM_SEG(s), (uint32_t)j * 4);
            m[(CARD_ROM_SEG(s) << 4) + (uint32_t)j * 4] = 0xCF;       /* IRET, if called directly */
        }
    }

    /* the CPU leaves the POST at the boot code with the BIOS stack */
    r = vm86_regs(pc->cpu);
    r->cs = ROM_SEG;
    r->ip = OFF_POST;
    r->ss = 0x0030;
    r->sp = 0x0100;
}

void pc_power_on(pc_t *pc)
{
    if (pc->b.powered) return;
    memset(&pc->b, 0, sizeof(pc->b));
    pc->b.powered = 1;
    pc->b.status = PC_RUNNING;
    pc->error[0] = 0;
    post(pc);
}

void pc_power_off(pc_t *pc)
{
    int s;
    if (!pc->b.powered) return;
    pc->b.powered = 0;
    pc->b.status = PC_OFF;
    if (pc->kbd) kbd_power(pc->kbd, 0);
    for (s = 0; s < PC_SLOTS; s++)
        if (pc->slot[s] && pc->slot[s]->power) pc->slot[s]->power(pc->slot[s]->user, 0);
}

void pc_reset(pc_t *pc) { pc_power_off(pc); pc_power_on(pc); }

pc_status_t pc_status(const pc_t *pc) { return (pc_status_t)pc->b.status; }
const char *pc_error(const pc_t *pc) { return pc->error; }

void pc_stop(pc_t *pc)
{
    if (pc->b.status != PC_RUNNING && pc->b.status != PC_HALTED) return;
    pc->b.status = PC_STOPPED;
    vm86_stop(pc->cpu);
}

void pc_set_speed(pc_t *pc, uint32_t ips) { if (ips) pc->ips = ips; }
uint32_t pc_speed(const pc_t *pc) { return pc->ips; }
uint64_t pc_time_us(const pc_t *pc) { return pc->b.time_us; }

void pc_set_int_hook(pc_t *pc, pc_int_hook_t hook, void *user) { pc->hook = hook; pc->hook_user = user; }

double pc_speaker_hz(const pc_t *pc)
{
    if (!pc->b.powered || (pc->b.port61 & 3) != 3) return 0.0;
    return pit_square_wave_hz(pc->pit, 2);
}

/* One 40 us slice: CPU, then every clock, then interrupts. */
static void slice(pc_t *pc)
{
    uint64_t steps;
    vm86_status_t st;
    int s, irq;

    pc->b.step_carry += (uint64_t)pc->ips * PC_SLICE_US;
    steps = pc->b.step_carry / 1000000u;
    pc->b.step_carry %= 1000000u;
    if (steps) vm86_run(pc->cpu, (uint32_t)steps);

    st = vm86_status(pc->cpu);
    if (st == VM86_ERR_UNSUPPORTED) {
        vm86_error_info_t e = vm86_error(pc->cpu);
        char msg[96];
        snprintf(msg, sizeof(msg), "unsupported instruction at %04X:%04X (opcode %02Xh)", e.cs, e.ip, e.opcode);
        fail(pc, msg);
        return;
    }
    if (st == VM86_STOPPED) {
        if (pc->b.status != PC_ERROR) pc->b.status = PC_STOPPED;
        return;
    }

    pc->b.time_us += PC_SLICE_US;
    pit_advance(pc->pit, PC_SLICE_US);
    if (pc->kbd) kbd_advance(pc->kbd, PC_SLICE_US);
    for (s = 0; s < PC_SLOTS; s++)
        if (pc->slot[s] && pc->slot[s]->advance) pc->slot[s]->advance(pc->slot[s]->user, PC_SLICE_US);

    if (pit_take_irq(pc->pit)) pic_raise(&pc->pic, 0);
    if (kbc_take_irq(pc->kbc)) pic_raise(&pc->pic, 1);
    for (s = 0; s < PC_SLOTS; s++)
        if (pc->slot[s] && pc->slot[s]->take_irq && (irq = pc->slot[s]->take_irq(pc->slot[s]->user)) >= 2 && irq <= 7)
            pic_raise(&pc->pic, irq);
    if (vm86_regs(pc->cpu)->flags & VM86_IF) {
        int vec = pic_acknowledge(&pc->pic);
        if (vec >= 0) vm86_request_interrupt(pc->cpu, (uint8_t)vec);
    }
    pc->b.status = (uint8_t)(vm86_status(pc->cpu) == VM86_HALTED ? PC_HALTED : PC_RUNNING);
}

void pc_run(pc_t *pc, double dt_seconds)
{
    uint64_t ns;
    if (!pc->b.powered || dt_seconds <= 0.0) return;
    ns = (uint64_t)(dt_seconds * 1e9) + pc->b.ns_carry;
    while (ns >= SLICE_NS && (pc->b.status == PC_RUNNING || pc->b.status == PC_HALTED)) {
        slice(pc);
        ns -= SLICE_NS;
    }
    pc->b.ns_carry = (pc->b.status == PC_RUNNING || pc->b.status == PC_HALTED) ? ns : 0;
}

int pc_load_com(pc_t *pc, const uint8_t *data, size_t len)
{
    if (!pc->b.powered || vm86_load_com(pc->cpu, data, len, PC_COM_SEGMENT) != 0) return -1;
    pc->pic.isr = 0;              /* whatever the BIOS was servicing is abandoned */
    pc->b.status = PC_RUNNING;
    pc->error[0] = 0;
    return 0;
}

/* ---------- state ---------- */

static size_t card_state_size(pc_card_t *c)
{
    return c && c->state_size && c->save_state && c->load_state ? c->state_size(c->user) : 0;
}

size_t pc_state_size(const pc_t *pc)
{
    size_t n = vm86_state_size() + sizeof(pic_t) + pit_state_size() + kbc_state_size() + sizeof(pc_board_t);
    int s;
    for (s = 0; s < PC_SLOTS; s++) n += card_state_size(pc->slot[s]);
    return n;
}

size_t pc_save_state(const pc_t *pc, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = pc_state_size(pc), n;
    int s;
    if (len < total) return 0;
    n = vm86_save_state(pc->cpu, p, len); p += n;
    memcpy(p, &pc->pic, sizeof(pic_t)); p += sizeof(pic_t);
    n = pit_save_state(pc->pit, p, pit_state_size()); p += n;
    n = kbc_save_state(pc->kbc, p, kbc_state_size()); p += n;
    memcpy(p, &pc->b, sizeof(pc_board_t)); p += sizeof(pc_board_t);
    for (s = 0; s < PC_SLOTS; s++) {
        size_t cs = card_state_size(pc->slot[s]);
        if (cs) { pc->slot[s]->save_state(pc->slot[s]->user, p, cs); p += cs; }
    }
    return total;
}

int pc_load_state(pc_t *pc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int s;
    if (len != pc_state_size(pc)) return -1;
    if (vm86_load_state(pc->cpu, p, vm86_state_size()) != 0) return -1;
    p += vm86_state_size();
    memcpy(&pc->pic, p, sizeof(pic_t)); p += sizeof(pic_t);
    if (pit_load_state(pc->pit, p, pit_state_size()) != 0) return -1;
    p += pit_state_size();
    if (kbc_load_state(pc->kbc, p, kbc_state_size()) != 0) return -1;
    p += kbc_state_size();
    memcpy(&pc->b, p, sizeof(pc_board_t)); p += sizeof(pc_board_t);
    for (s = 0; s < PC_SLOTS; s++) {
        size_t cs = card_state_size(pc->slot[s]);
        if (cs) { if (pc->slot[s]->load_state(pc->slot[s]->user, p, cs) != 0) return -1; p += cs; }
    }
    return 0;
}
