/*
 * vm86 - 8086 CPU core (subset). See include/vm86.h.
 *
 * Included: the whole 8086 except what is listed under "Not supported".
 * Not supported (stops the CPU with VM86_ERR_UNSUPPORTED):
 *   BCD (DAA DAS AAA AAS AAM AAD), XLAT, LOCK, WAIT, ESC (FPU), INTO,
 *   POP CS, MOV CS, aliases and undocumented opcodes (60-6F, 82, C0, C1,
 *   C8, C9, D6, F1, F6/F7 /1, D0-D3 /6, etc.) and LEA, LDS, LES and
 *   far CALL/JMP through memory (FF /3, FF /5) with a register operand.
 * Not emulated: cycle timing, prefetch queue, TF single-step trap.
 */
#include "vm86.h"

#include <stdlib.h>
#include <string.h>

#define FLAGS_MASK  0x0FD5u   /* flag bits that exist on the 8086 */
#define FLAGS_FIXED 0xF002u   /* on the 8086 bits 12-15 and bit 1 read as 1 */
#define IRQ_QUEUE   16

typedef struct vm86_state {
    vm86_regs_t regs;
    uint8_t status;
    uint8_t inhibit_irq;          /* after STI / MOV SS / POP SS */
    uint8_t irq_count;
    uint8_t irq_queue[IRQ_QUEUE];
    vm86_error_info_t err;
    uint8_t mem[VM86_MEM_SIZE];
} vm86_state_t;

struct vm86 {
    vm86_state_t st;
    vm86_host_t host;
};

/* Context of the instruction being executed */
typedef struct {
    vm86_t *vm;
    int seg_ovr;                  /* -1 or segment index */
    uint8_t rep;                  /* 0, 0xF2, 0xF3 */
    uint16_t start_ip;
    uint8_t op;
    /* ModRM */
    uint8_t mod, reg, rm;
    uint16_t ea_seg, ea_off;
} ctx_t;

enum { ES_ = 0, CS_ = 1, SS_ = 2, DS_ = 3 };

/* ---------- register access ---------- */

static uint16_t *r16p(vm86_t *vm, int i)
{
    vm86_regs_t *r = &vm->st.regs;
    switch (i & 7) {
    case 0: return &r->ax; case 1: return &r->cx;
    case 2: return &r->dx; case 3: return &r->bx;
    case 4: return &r->sp; case 5: return &r->bp;
    case 6: return &r->si; default: return &r->di;
    }
}

static uint16_t *sregp(vm86_t *vm, int i)
{
    vm86_regs_t *r = &vm->st.regs;
    switch (i & 3) {
    case 0: return &r->es; case 1: return &r->cs;
    case 2: return &r->ss; default: return &r->ds;
    }
}

static uint8_t get_r8(vm86_t *vm, int i)
{
    uint16_t v = *r16p(vm, i & 3);
    return (i & 4) ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static void set_r8(vm86_t *vm, int i, uint8_t v)
{
    uint16_t *p = r16p(vm, i & 3);
    if (i & 4) *p = (uint16_t)((*p & 0x00FF) | (v << 8));
    else       *p = (uint16_t)((*p & 0xFF00) | v);
}

#define R  (vm->st.regs)
#define F  (vm->st.regs.flags)
#define GETF(f) ((F & (f)) != 0)

static void setf(vm86_t *vm, uint16_t f, int on)
{
    if (on) F |= f; else F &= (uint16_t)~f;
}

static uint16_t norm_flags(uint16_t f) { return (uint16_t)((f & FLAGS_MASK) | FLAGS_FIXED); }

/* ---------- memory ---------- */

static uint32_t lin(uint16_t seg, uint16_t off)
{
    return (((uint32_t)seg << 4) + off) & (VM86_MEM_SIZE - 1);
}

static uint8_t rd8(vm86_t *vm, uint16_t seg, uint16_t off) { return vm->st.mem[lin(seg, off)]; }
static void wr8(vm86_t *vm, uint16_t seg, uint16_t off, uint8_t v) { vm->st.mem[lin(seg, off)] = v; }

static uint16_t rd16(vm86_t *vm, uint16_t seg, uint16_t off)
{
    return (uint16_t)(rd8(vm, seg, off) | (rd8(vm, seg, (uint16_t)(off + 1)) << 8));
}

static void wr16(vm86_t *vm, uint16_t seg, uint16_t off, uint16_t v)
{
    wr8(vm, seg, off, (uint8_t)v);
    wr8(vm, seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
}

static uint8_t fetch8(vm86_t *vm) { return rd8(vm, R.cs, R.ip++); }

static uint16_t fetch16(vm86_t *vm)
{
    uint16_t lo = fetch8(vm);
    return (uint16_t)(lo | (fetch8(vm) << 8));
}

static void push16(vm86_t *vm, uint16_t v) { R.sp -= 2; wr16(vm, R.ss, R.sp, v); }

static uint16_t pop16(vm86_t *vm)
{
    uint16_t v = rd16(vm, R.ss, R.sp);
    R.sp += 2;
    return v;
}

/* ---------- I/O ports ---------- */

static uint8_t port_in8(vm86_t *vm, uint16_t p)
{
    return vm->host.in8 ? vm->host.in8(vm->host.user, p) : 0xFF;
}

static void port_out8(vm86_t *vm, uint16_t p, uint8_t v)
{
    if (vm->host.out8) vm->host.out8(vm->host.user, p, v);
}

/* ---------- interrupts ---------- */

static void do_interrupt(vm86_t *vm, uint8_t n)
{
    if (vm->host.interrupt && vm->host.interrupt(vm->host.user, vm, n))
        return;
    push16(vm, norm_flags(F));
    F &= (uint16_t)~(VM86_IF | VM86_TF);
    push16(vm, R.cs);
    push16(vm, R.ip);
    R.ip = rd16(vm, 0, (uint16_t)(n * 4));
    R.cs = rd16(vm, 0, (uint16_t)(n * 4 + 2));
}

/* ---------- ModRM ---------- */

static void decode_modrm(ctx_t *c)
{
    vm86_t *vm = c->vm;
    uint8_t m = fetch8(vm);
    uint16_t off = 0;
    int seg = DS_;

    c->mod = m >> 6;
    c->reg = (m >> 3) & 7;
    c->rm = m & 7;
    if (c->mod == 3) return;

    switch (c->rm) {
    case 0: off = (uint16_t)(R.bx + R.si); break;
    case 1: off = (uint16_t)(R.bx + R.di); break;
    case 2: off = (uint16_t)(R.bp + R.si); seg = SS_; break;
    case 3: off = (uint16_t)(R.bp + R.di); seg = SS_; break;
    case 4: off = R.si; break;
    case 5: off = R.di; break;
    case 6:
        if (c->mod == 0) off = fetch16(vm);
        else { off = R.bp; seg = SS_; }
        break;
    case 7: off = R.bx; break;
    }
    if (c->mod == 1) off = (uint16_t)(off + (int8_t)fetch8(vm));
    else if (c->mod == 2) off = (uint16_t)(off + fetch16(vm));

    if (c->seg_ovr >= 0) seg = c->seg_ovr;
    c->ea_seg = *sregp(vm, seg);
    c->ea_off = off;
}

static uint16_t get_e(ctx_t *c, int w)
{
    vm86_t *vm = c->vm;
    if (c->mod == 3) return w ? *r16p(vm, c->rm) : get_r8(vm, c->rm);
    return w ? rd16(vm, c->ea_seg, c->ea_off) : rd8(vm, c->ea_seg, c->ea_off);
}

static void set_e(ctx_t *c, int w, uint16_t v)
{
    vm86_t *vm = c->vm;
    if (c->mod == 3) {
        if (w) *r16p(vm, c->rm) = v; else set_r8(vm, c->rm, (uint8_t)v);
    } else {
        if (w) wr16(vm, c->ea_seg, c->ea_off, v); else wr8(vm, c->ea_seg, c->ea_off, (uint8_t)v);
    }
}

static uint16_t get_g(ctx_t *c, int w)
{
    return w ? *r16p(c->vm, c->reg) : get_r8(c->vm, c->reg);
}

static void set_g(ctx_t *c, int w, uint16_t v)
{
    if (w) *r16p(c->vm, c->reg) = v; else set_r8(c->vm, c->reg, (uint8_t)v);
}

static uint16_t data_seg(ctx_t *c)
{
    return *sregp(c->vm, c->seg_ovr >= 0 ? c->seg_ovr : DS_);
}

/* ---------- ALU ---------- */

static int parity8(uint8_t v)
{
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}

static void set_szp(vm86_t *vm, uint16_t r, int w)
{
    uint16_t mask = w ? 0xFFFF : 0xFF;
    uint16_t sign = w ? 0x8000 : 0x80;
    r &= mask;
    setf(vm, VM86_ZF, r == 0);
    setf(vm, VM86_SF, (r & sign) != 0);
    setf(vm, VM86_PF, parity8((uint8_t)r));
}

static uint16_t do_add(vm86_t *vm, uint16_t a, uint16_t b, int carry, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t r = (uint32_t)a + b + (uint32_t)carry;
    setf(vm, VM86_CF, r > mask);
    setf(vm, VM86_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(vm, VM86_OF, ((r ^ a) & (r ^ b) & sign) != 0);
    set_szp(vm, (uint16_t)r, w);
    return (uint16_t)(r & mask);
}

static uint16_t do_sub(vm86_t *vm, uint16_t a, uint16_t b, int borrow, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t r = ((uint32_t)a - b - (uint32_t)borrow) & 0x1FFFF;
    setf(vm, VM86_CF, (uint32_t)a < (uint32_t)b + (uint32_t)borrow);
    setf(vm, VM86_AF, ((a ^ b ^ r) & 0x10) != 0);
    setf(vm, VM86_OF, ((a ^ b) & (a ^ r) & sign) != 0);
    set_szp(vm, (uint16_t)r, w);
    return (uint16_t)(r & mask);
}

static uint16_t do_logic(vm86_t *vm, uint16_t r, int w)
{
    setf(vm, VM86_CF | VM86_OF | VM86_AF, 0);
    set_szp(vm, r, w);
    return (uint16_t)(r & (w ? 0xFFFF : 0xFF));
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static uint16_t alu(vm86_t *vm, int op, uint16_t a, uint16_t b, int w)
{
    switch (op) {
    case 0: return do_add(vm, a, b, 0, w);
    case 1: return do_logic(vm, a | b, w);
    case 2: return do_add(vm, a, b, GETF(VM86_CF), w);
    case 3: return do_sub(vm, a, b, GETF(VM86_CF), w);
    case 4: return do_logic(vm, a & b, w);
    case 5: return do_sub(vm, a, b, 0, w);
    case 6: return do_logic(vm, a ^ b, w);
    default: do_sub(vm, a, b, 0, w); return a; /* CMP */
    }
}

static uint16_t do_inc_dec(vm86_t *vm, uint16_t v, int dec, int w)
{
    int cf = GETF(VM86_CF);
    uint16_t r = dec ? do_sub(vm, v, 1, 0, w) : do_add(vm, v, 1, 0, w);
    setf(vm, VM86_CF, cf);
    return r;
}

/* Shifts and rotates. op: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 7 SAR */
static uint16_t do_shift(vm86_t *vm, int op, uint16_t v, unsigned count, int w)
{
    uint16_t mask = w ? 0xFFFF : 0xFF;
    uint16_t sign = w ? 0x8000 : 0x80;
    int cf = GETF(VM86_CF), of = GETF(VM86_OF);
    unsigned i;

    if (count == 0) return v;
    v &= mask;
    for (i = 0; i < count; i++) {
        uint16_t prev = v;
        switch (op) {
        case 0: cf = (v & sign) != 0; v = (uint16_t)(((v << 1) | cf) & mask);
                of = ((v & sign) != 0) ^ cf; break;
        case 1: cf = v & 1; v = (uint16_t)((v >> 1) | (cf ? sign : 0));
                of = ((v ^ (v << 1)) & sign) != 0; break;
        case 2: { int nc = (v & sign) != 0; v = (uint16_t)(((v << 1) | cf) & mask); cf = nc;
                of = ((v & sign) != 0) ^ cf; break; }
        case 3: { int nc = v & 1; v = (uint16_t)((v >> 1) | (cf ? sign : 0)); cf = nc;
                of = ((v ^ (v << 1)) & sign) != 0; break; }
        case 4: cf = (v & sign) != 0; v = (uint16_t)((v << 1) & mask);
                of = ((v & sign) != 0) ^ cf; break;
        case 5: cf = v & 1; v = (uint16_t)(v >> 1);
                of = (prev & sign) != 0; break;
        case 7: cf = v & 1; v = (uint16_t)((v >> 1) | (v & sign));
                of = 0; break;
        }
    }
    setf(vm, VM86_CF, cf);
    setf(vm, VM86_OF, of);
    if (op >= 4) {
        set_szp(vm, v, w);
        setf(vm, VM86_AF, 0);
    }
    return v;
}

static int cond(vm86_t *vm, int cc)
{
    int r;
    switch (cc >> 1) {
    case 0: r = GETF(VM86_OF); break;
    case 1: r = GETF(VM86_CF); break;
    case 2: r = GETF(VM86_ZF); break;
    case 3: r = GETF(VM86_CF) || GETF(VM86_ZF); break;
    case 4: r = GETF(VM86_SF); break;
    case 5: r = GETF(VM86_PF); break;
    case 6: r = GETF(VM86_SF) != GETF(VM86_OF); break;
    default: r = GETF(VM86_ZF) || (GETF(VM86_SF) != GETF(VM86_OF)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---------- MUL / DIV (group 3) ---------- */

/* Returns 0 on success, 1 on a divide error. */
static int do_muldiv(ctx_t *c, int op, int w)
{
    vm86_t *vm = c->vm;
    uint16_t src = get_e(c, w);

    if (!w) {
        switch (op) {
        case 4: { uint16_t r = (uint16_t)(get_r8(vm, 0) * (uint8_t)src);
                  R.ax = r; setf(vm, VM86_CF | VM86_OF, (r >> 8) != 0);
                  set_szp(vm, r >> 8, 0); return 0; }
        case 5: { int16_t r = (int16_t)((int8_t)get_r8(vm, 0) * (int8_t)src);
                  R.ax = (uint16_t)r; setf(vm, VM86_CF | VM86_OF, r != (int8_t)r);
                  set_szp(vm, (uint16_t)r >> 8, 0); return 0; }
        case 6: { uint16_t q;
                  if ((uint8_t)src == 0) return 1;
                  q = R.ax / (uint8_t)src;
                  if (q > 0xFF) return 1;
                  R.ax = (uint16_t)(((R.ax % (uint8_t)src) << 8) | q); return 0; }
        default: { int16_t a = (int16_t)R.ax; int8_t d = (int8_t)src; int q, rem;
                  if (d == 0) return 1;
                  q = a / d; rem = a % d;
                  if (q > 127 || q < -127) return 1;
                  if (c->rep) q = -q; /* 8086 quirk: REP + IDIV negates the quotient */
                  R.ax = (uint16_t)(((uint8_t)rem << 8) | (uint8_t)q); return 0; }
        }
    } else {
        switch (op) {
        case 4: { uint32_t r = (uint32_t)R.ax * src;
                  R.ax = (uint16_t)r; R.dx = (uint16_t)(r >> 16);
                  setf(vm, VM86_CF | VM86_OF, R.dx != 0);
                  set_szp(vm, R.dx, 1); return 0; }
        case 5: { int32_t r = (int32_t)(int16_t)R.ax * (int16_t)src;
                  R.ax = (uint16_t)r; R.dx = (uint16_t)((uint32_t)r >> 16);
                  setf(vm, VM86_CF | VM86_OF, r != (int16_t)r);
                  set_szp(vm, R.dx, 1); return 0; }
        case 6: { uint32_t a = ((uint32_t)R.dx << 16) | R.ax, q;
                  if (src == 0) return 1;
                  q = a / src;
                  if (q > 0xFFFF) return 1;
                  R.dx = (uint16_t)(a % src); R.ax = (uint16_t)q; return 0; }
        default: { int32_t a = (int32_t)(((uint32_t)R.dx << 16) | R.ax); int16_t d = (int16_t)src;
                  int32_t q, rem;
                  if (d == 0) return 1;
                  if (a == INT32_MIN && d == -1) return 1;
                  q = a / d; rem = a % d;
                  if (q > 32767 || q < -32767) return 1;
                  if (c->rep) q = -q;
                  R.ax = (uint16_t)q; R.dx = (uint16_t)rem; return 0; }
        }
    }
}

/* ---------- string instructions ---------- */

static void string_op(ctx_t *c)
{
    vm86_t *vm = c->vm;
    int w = c->op & 1;
    int16_t d = (int16_t)(GETF(VM86_DF) ? -(1 + w) : (1 + w));
    uint16_t sseg = data_seg(c);
    int is_cmp = 0;

    if (c->rep && R.cx == 0) return;

    switch (c->op & ~1) {
    case 0xA4: /* MOVS */
        if (w) wr16(vm, R.es, R.di, rd16(vm, sseg, R.si));
        else   wr8(vm, R.es, R.di, rd8(vm, sseg, R.si));
        R.si += d; R.di += d; break;
    case 0xA6: /* CMPS */
        alu(vm, 7, w ? rd16(vm, sseg, R.si) : rd8(vm, sseg, R.si),
                   w ? rd16(vm, R.es, R.di) : rd8(vm, R.es, R.di), w);
        R.si += d; R.di += d; is_cmp = 1; break;
    case 0xAA: /* STOS */
        if (w) wr16(vm, R.es, R.di, R.ax); else wr8(vm, R.es, R.di, (uint8_t)R.ax);
        R.di += d; break;
    case 0xAC: /* LODS */
        if (w) R.ax = rd16(vm, sseg, R.si); else set_r8(vm, 0, rd8(vm, sseg, R.si));
        R.si += d; break;
    case 0xAE: /* SCAS */
        alu(vm, 7, w ? R.ax : get_r8(vm, 0), w ? rd16(vm, R.es, R.di) : rd8(vm, R.es, R.di), w);
        R.di += d; is_cmp = 1; break;
    }

    if (c->rep) {
        R.cx--;
        if (R.cx == 0) return;
        if (is_cmp && c->rep == 0xF3 && !GETF(VM86_ZF)) return;
        if (is_cmp && c->rep == 0xF2 && GETF(VM86_ZF)) return;
        R.ip = c->start_ip; /* next iteration on the next step */
    }
}

/* ---------- single step ---------- */

static void unsupported(ctx_t *c, uint8_t reg)
{
    vm86_t *vm = c->vm;
    vm->st.err.cs = R.cs;
    vm->st.err.ip = c->start_ip;
    vm->st.err.opcode = c->op;
    vm->st.err.modrm_reg = reg;
    vm->st.status = VM86_ERR_UNSUPPORTED;
    R.ip = c->start_ip;
}

static void step(vm86_t *vm)
{
    ctx_t c;
    int w, prefixes = 0;
    uint16_t a, b;

    c.vm = vm;
    c.seg_ovr = -1;
    c.rep = 0;
    c.start_ip = R.ip;
    c.mod = 3; c.reg = c.rm = 0;

    for (;;) {
        c.op = fetch8(vm);
        if (c.op == 0x26 || c.op == 0x2E || c.op == 0x36 || c.op == 0x3E) c.seg_ovr = (c.op >> 3) & 3;
        else if (c.op == 0xF2 || c.op == 0xF3) c.rep = c.op;
        else break;
        if (++prefixes > 15) { unsupported(&c, 0xFF); return; }
    }

    w = c.op & 1;

    /* Classic ALU block 00-3F: op&7 in 0..5 */
    if (c.op < 0x40 && (c.op & 7) < 6) {
        int aop = c.op >> 3;
        switch (c.op & 7) {
        case 0: case 1: decode_modrm(&c);
            a = alu(vm, aop, get_e(&c, w), get_g(&c, w), w);
            if (aop != 7) set_e(&c, w, a);
            return;
        case 2: case 3: decode_modrm(&c);
            a = alu(vm, aop, get_g(&c, w), get_e(&c, w), w);
            if (aop != 7) set_g(&c, w, a);
            return;
        case 4: a = alu(vm, aop, get_r8(vm, 0), fetch8(vm), 0);
            set_r8(vm, 0, (uint8_t)a); return;
        case 5: a = alu(vm, aop, R.ax, fetch16(vm), 1);
            R.ax = a; return;
        }
    }

    switch (c.op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        push16(vm, *sregp(vm, c.op >> 3)); return;
    case 0x07: case 0x17: case 0x1F:
        *sregp(vm, c.op >> 3) = pop16(vm);
        if (c.op == 0x17) vm->st.inhibit_irq = 1;
        return;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        *r16p(vm, c.op) = do_inc_dec(vm, *r16p(vm, c.op), 0, 1); return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        *r16p(vm, c.op) = do_inc_dec(vm, *r16p(vm, c.op), 1, 1); return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (c.op == 0x54) { R.sp -= 2; wr16(vm, R.ss, R.sp, R.sp); } /* 8086: pushes the already decremented SP */
        else push16(vm, *r16p(vm, c.op));
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        *r16p(vm, c.op) = pop16(vm); return;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(vm);
        if (cond(vm, c.op & 15)) R.ip = (uint16_t)(R.ip + rel);
        return;
    }

    case 0x80: case 0x81: case 0x83:
        decode_modrm(&c);
        w = (c.op != 0x80);
        a = get_e(&c, w);
        if (c.op == 0x81) b = fetch16(vm);
        else if (c.op == 0x83) b = (uint16_t)(int16_t)(int8_t)fetch8(vm);
        else b = fetch8(vm);
        a = alu(vm, c.reg, a, b, w);
        if (c.reg != 7) set_e(&c, w, a);
        return;

    case 0x84: case 0x85:
        decode_modrm(&c);
        do_logic(vm, get_e(&c, w) & get_g(&c, w), w);
        return;
    case 0x86: case 0x87:
        decode_modrm(&c);
        a = get_e(&c, w); b = get_g(&c, w);
        set_g(&c, w, a); set_e(&c, w, b);
        return;
    case 0x88: case 0x89: decode_modrm(&c); set_e(&c, w, get_g(&c, w)); return;
    case 0x8A: case 0x8B: decode_modrm(&c); set_g(&c, w, get_e(&c, w)); return;
    case 0x8C:
        decode_modrm(&c);
        if (c.reg > 3) { unsupported(&c, c.reg); return; }
        set_e(&c, 1, *sregp(vm, c.reg));
        return;
    case 0x8D:
        decode_modrm(&c);
        if (c.mod == 3) { unsupported(&c, c.reg); return; }
        *r16p(vm, c.reg) = c.ea_off;
        return;
    case 0x8E:
        decode_modrm(&c);
        if (c.reg > 3 || c.reg == CS_) { unsupported(&c, c.reg); return; }
        *sregp(vm, c.reg) = get_e(&c, 1);
        if (c.reg == SS_) vm->st.inhibit_irq = 1;
        return;
    case 0x8F:
        decode_modrm(&c);
        if (c.reg != 0) { unsupported(&c, c.reg); return; }
        a = pop16(vm);
        set_e(&c, 1, a);
        return;

    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        a = R.ax; R.ax = *r16p(vm, c.op); *r16p(vm, c.op) = a; return;
    case 0x98: R.ax = (uint16_t)(int16_t)(int8_t)get_r8(vm, 0); return;
    case 0x99: R.dx = (R.ax & 0x8000) ? 0xFFFF : 0; return;
    case 0x9A: a = fetch16(vm); b = fetch16(vm); push16(vm, R.cs); push16(vm, R.ip); R.cs = b; R.ip = a; return;
    case 0x9C: push16(vm, norm_flags(F)); return;
    case 0x9D: F = norm_flags(pop16(vm)); return;
    case 0x9E: F = norm_flags((uint16_t)((F & 0xFF00) | (get_r8(vm, 4) & 0xD5))); return;
    case 0x9F: set_r8(vm, 4, (uint8_t)norm_flags(F)); return;

    case 0xA0: a = fetch16(vm); set_r8(vm, 0, rd8(vm, data_seg(&c), a)); return;
    case 0xA1: a = fetch16(vm); R.ax = rd16(vm, data_seg(&c), a); return;
    case 0xA2: a = fetch16(vm); wr8(vm, data_seg(&c), a, get_r8(vm, 0)); return;
    case 0xA3: a = fetch16(vm); wr16(vm, data_seg(&c), a, R.ax); return;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(&c); return;
    case 0xA8: do_logic(vm, get_r8(vm, 0) & fetch8(vm), 0); return;
    case 0xA9: do_logic(vm, R.ax & fetch16(vm), 1); return;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_r8(vm, c.op & 7, fetch8(vm)); return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        *r16p(vm, c.op) = fetch16(vm); return;

    case 0xC2: a = fetch16(vm); R.ip = pop16(vm); R.sp = (uint16_t)(R.sp + a); return;
    case 0xC3: R.ip = pop16(vm); return;
    case 0xC4: case 0xC5:
        decode_modrm(&c);
        if (c.mod == 3) { unsupported(&c, c.reg); return; }
        a = rd16(vm, c.ea_seg, c.ea_off);
        b = rd16(vm, c.ea_seg, (uint16_t)(c.ea_off + 2));
        *r16p(vm, c.reg) = a;
        *sregp(vm, c.op == 0xC4 ? ES_ : DS_) = b;
        return;
    case 0xC6: case 0xC7:
        decode_modrm(&c);
        if (c.reg != 0) { unsupported(&c, c.reg); return; }
        set_e(&c, w, w ? fetch16(vm) : fetch8(vm));
        return;
    case 0xCA: a = fetch16(vm); R.ip = pop16(vm); R.cs = pop16(vm); R.sp = (uint16_t)(R.sp + a); return;
    case 0xCB: R.ip = pop16(vm); R.cs = pop16(vm); return;
    case 0xCC: do_interrupt(vm, 3); return;
    case 0xCD: do_interrupt(vm, fetch8(vm)); return;
    case 0xCF: R.ip = pop16(vm); R.cs = pop16(vm); F = norm_flags(pop16(vm)); return;

    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        decode_modrm(&c);
        if (c.reg == 6) { unsupported(&c, c.reg); return; }
        a = get_e(&c, w);
        a = do_shift(vm, c.reg, a, (c.op & 2) ? (R.cx & 0xFF) : 1, w);
        set_e(&c, w, a);
        return;

    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = (int8_t)fetch8(vm);
        int jump;
        R.cx--;
        jump = R.cx != 0;
        if (c.op == 0xE0) jump = jump && !GETF(VM86_ZF);
        if (c.op == 0xE1) jump = jump && GETF(VM86_ZF);
        if (jump) R.ip = (uint16_t)(R.ip + rel);
        return;
    }
    case 0xE3: { int8_t rel = (int8_t)fetch8(vm); if (R.cx == 0) R.ip = (uint16_t)(R.ip + rel); return; }
    case 0xE4: set_r8(vm, 0, port_in8(vm, fetch8(vm))); return;
    case 0xE5: a = fetch8(vm); R.ax = (uint16_t)(port_in8(vm, a) | (port_in8(vm, (uint16_t)(a + 1)) << 8)); return;
    case 0xE6: port_out8(vm, fetch8(vm), get_r8(vm, 0)); return;
    case 0xE7: a = fetch8(vm); port_out8(vm, a, (uint8_t)R.ax); port_out8(vm, (uint16_t)(a + 1), (uint8_t)(R.ax >> 8)); return;
    case 0xE8: a = fetch16(vm); push16(vm, R.ip); R.ip = (uint16_t)(R.ip + a); return;
    case 0xE9: a = fetch16(vm); R.ip = (uint16_t)(R.ip + a); return;
    case 0xEA: a = fetch16(vm); b = fetch16(vm); R.cs = b; R.ip = a; return;
    case 0xEB: { int8_t rel = (int8_t)fetch8(vm); R.ip = (uint16_t)(R.ip + rel); return; }
    case 0xEC: set_r8(vm, 0, port_in8(vm, R.dx)); return;
    case 0xED: R.ax = (uint16_t)(port_in8(vm, R.dx) | (port_in8(vm, (uint16_t)(R.dx + 1)) << 8)); return;
    case 0xEE: port_out8(vm, R.dx, get_r8(vm, 0)); return;
    case 0xEF: port_out8(vm, R.dx, (uint8_t)R.ax); port_out8(vm, (uint16_t)(R.dx + 1), (uint8_t)(R.ax >> 8)); return;

    case 0xF4: vm->st.status = VM86_HALTED; return;
    case 0xF5: F ^= VM86_CF; return;
    case 0xF6: case 0xF7:
        decode_modrm(&c);
        switch (c.reg) {
        case 0: do_logic(vm, get_e(&c, w) & (w ? fetch16(vm) : fetch8(vm)), w); return;
        case 1: unsupported(&c, c.reg); return;
        case 2: set_e(&c, w, (uint16_t)~get_e(&c, w)); return;
        case 3: a = get_e(&c, w); set_e(&c, w, do_sub(vm, 0, a, 0, w)); setf(vm, VM86_CF, a != 0); return;
        default:
            if (do_muldiv(&c, c.reg, w)) do_interrupt(vm, 0);
            return;
        }
    case 0xF8: setf(vm, VM86_CF, 0); return;
    case 0xF9: setf(vm, VM86_CF, 1); return;
    case 0xFA: setf(vm, VM86_IF, 0); return;
    case 0xFB: if (!GETF(VM86_IF)) vm->st.inhibit_irq = 1; setf(vm, VM86_IF, 1); return;
    case 0xFC: setf(vm, VM86_DF, 0); return;
    case 0xFD: setf(vm, VM86_DF, 1); return;
    case 0xFE:
        decode_modrm(&c);
        if (c.reg > 1) { unsupported(&c, c.reg); return; }
        set_e(&c, 0, do_inc_dec(vm, get_e(&c, 0), c.reg, 0));
        return;
    case 0xFF:
        decode_modrm(&c);
        switch (c.reg) {
        case 0: case 1: set_e(&c, 1, do_inc_dec(vm, get_e(&c, 1), c.reg, 1)); return;
        case 2: a = get_e(&c, 1); push16(vm, R.ip); R.ip = a; return;
        case 3: case 5: /* far: offset, then segment */
            if (c.mod == 3) { unsupported(&c, c.reg); return; }
            a = rd16(vm, c.ea_seg, c.ea_off);
            b = rd16(vm, c.ea_seg, (uint16_t)(c.ea_off + 2));
            if (c.reg == 3) { push16(vm, R.cs); push16(vm, R.ip); }
            R.cs = b; R.ip = a;
            return;
        case 4: R.ip = get_e(&c, 1); return;
        case 6:
            if (c.mod == 3 && c.rm == 4) { R.sp -= 2; wr16(vm, R.ss, R.sp, R.sp); }
            else { a = get_e(&c, 1); push16(vm, a); }
            return;
        default: unsupported(&c, c.reg); return;
        }
    default:
        unsupported(&c, 0xFF);
        return;
    }
}

/* ---------- public API ---------- */

vm86_t *vm86_create(void)
{
    vm86_t *vm = (vm86_t *)calloc(1, sizeof(vm86_t));
    if (vm) vm86_reset(vm);
    return vm;
}

void vm86_destroy(vm86_t *vm) { free(vm); }

void vm86_reset(vm86_t *vm)
{
    memset(&vm->st.regs, 0, sizeof(vm->st.regs));
    vm->st.regs.cs = 0xFFFF;
    vm->st.regs.flags = FLAGS_FIXED;
    vm->st.status = VM86_RUNNING;
    vm->st.inhibit_irq = 0;
    vm->st.irq_count = 0;
    memset(&vm->st.err, 0, sizeof(vm->st.err));
}

void vm86_set_host(vm86_t *vm, const vm86_host_t *host)
{
    if (host) vm->host = *host; else memset(&vm->host, 0, sizeof(vm->host));
}

uint8_t *vm86_memory(vm86_t *vm) { return vm->st.mem; }
vm86_regs_t *vm86_regs(vm86_t *vm) { return &vm->st.regs; }

int vm86_load_com(vm86_t *vm, const uint8_t *data, size_t len, uint16_t seg)
{
    uint32_t base = (uint32_t)seg << 4;
    if (len > 0xFF00 || base + 0x10000 > VM86_MEM_SIZE) return -1;
    memset(vm->st.mem + base, 0, 0x10000);
    vm->st.mem[base + 0] = 0xCD;   /* PSP: INT 20h at seg:0000 */
    vm->st.mem[base + 1] = 0x20;
    memcpy(vm->st.mem + base + 0x100, data, len);
    vm86_reset(vm);
    R.cs = R.ds = R.es = R.ss = seg;
    R.ip = 0x100;
    R.sp = 0xFFFE;                 /* word 0 on top: RET -> seg:0000 */
    R.flags = norm_flags(VM86_IF);
    return 0;
}

uint32_t vm86_run(vm86_t *vm, uint32_t max_steps)
{
    uint32_t n = 0;
    while (n < max_steps) {
        if (vm->st.inhibit_irq) {
            vm->st.inhibit_irq = 0;
        } else if (vm->st.irq_count && (GETF(VM86_IF) || vm->st.status == VM86_HALTED)) {
            if (GETF(VM86_IF)) {
                uint8_t num = vm->st.irq_queue[0];
                memmove(vm->st.irq_queue, vm->st.irq_queue + 1, --vm->st.irq_count);
                if (vm->st.status == VM86_HALTED) vm->st.status = VM86_RUNNING;
                do_interrupt(vm, num);
            }
        }
        if (vm->st.status != VM86_RUNNING) break;
        step(vm);
        n++;
    }
    return n;
}

vm86_status_t vm86_status(const vm86_t *vm) { return (vm86_status_t)vm->st.status; }
void vm86_stop(vm86_t *vm) { vm->st.status = VM86_STOPPED; }
void vm86_resume(vm86_t *vm) { vm->st.status = VM86_RUNNING; }
vm86_error_info_t vm86_error(const vm86_t *vm) { return vm->st.err; }

int vm86_request_interrupt(vm86_t *vm, uint8_t number)
{
    if (vm->st.irq_count >= IRQ_QUEUE) return -1;
    vm->st.irq_queue[vm->st.irq_count++] = number;
    return 0;
}

size_t vm86_state_size(void) { return sizeof(vm86_state_t); }

size_t vm86_save_state(const vm86_t *vm, void *buf, size_t len)
{
    if (len < sizeof(vm86_state_t)) return 0;
    memcpy(buf, &vm->st, sizeof(vm86_state_t));
    return sizeof(vm86_state_t);
}

int vm86_load_state(vm86_t *vm, const void *buf, size_t len)
{
    if (len != sizeof(vm86_state_t)) return -1;
    memcpy(&vm->st, buf, sizeof(vm86_state_t));
    return 0;
}
