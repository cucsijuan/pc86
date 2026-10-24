/*
 * asm86 - assembler for a subset of NASM targeting the 8086. See include/asm86.h.
 *
 * How it works: the text is tokenized and parsed into statements once.
 * Then several passes compute addresses and sizes until nothing changes
 * (like NASM's multi-pass optimizer, which picks the shortest form of each
 * jump and immediate). A final pass emits the bytes and reports errors.
 *
 * Sections, as in NASM's flat binary output: .text first, then .data and
 * .bss, each starting at an address that is a multiple of 4. .bss takes
 * addresses but no room in the output. Each pass places them with the
 * sizes the previous pass found.
 */
#include "asm86.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PASSES   40
#define STICKY_PASS  12   /* from this pass on, long forms never shrink again */

/* ====================================================================== */
/* Tokens                                                                  */
/* ====================================================================== */

enum { TK_EOL, TK_ID, TK_NUM, TK_STR, TK_PUNCT, TK_DOLLAR, TK_DOLLAR2 };

/* Two-character punctuation */
#define P_SHL 'L'   /* << */
#define P_SHR 'R'   /* >> */
#define P_SDIV 'D'  /* // */
#define P_SMOD 'M'  /* %% */

typedef struct {
    uint8_t type;
    char punct;
    int len;
    const char *s;
    int64_t num;
} tok_t;

/* ====================================================================== */
/* Keywords                                                                */
/* ====================================================================== */

enum {
    M_NONE = 0,
    /* ALU: the order matches the 8086 /r field */
    M_ADD, M_OR, M_ADC, M_SBB, M_AND, M_SUB, M_XOR, M_CMP,
    M_MOV, M_XCHG, M_TEST, M_LEA,
    M_INC, M_DEC, M_NOT, M_NEG, M_MUL, M_IMUL, M_DIV, M_IDIV,
    M_ROL, M_ROR, M_RCL, M_RCR, M_SHL, M_SHR, M_SAR,
    M_PUSH, M_POP,
    M_JMP, M_CALL, M_RET, M_RETF, M_IRET, M_INT, M_INT3,
    M_LDS,  /* LDS/LES, opcode in cc_or_byte */
    M_JCC,  /* + condition code in cc_or_byte */
    M_LOOP, M_LOOPE, M_LOOPNE, M_JCXZ,
    M_IN, M_OUT,
    M_SIMPLE /* one-byte instructions without operands, opcode in cc_or_byte */
};

typedef struct {
    const char *name;
    int id;
    int cc_or_byte;   /* condition (Jcc), /r (shifts, group 3) or opcode (simple) */
    int is_string;    /* accepts a REP prefix */
} mnem_t;

static const mnem_t MNEMS[] = {
    {"add", M_ADD, 0, 0}, {"or", M_OR, 1, 0}, {"adc", M_ADC, 2, 0}, {"sbb", M_SBB, 3, 0},
    {"and", M_AND, 4, 0}, {"sub", M_SUB, 5, 0}, {"xor", M_XOR, 6, 0}, {"cmp", M_CMP, 7, 0},
    {"mov", M_MOV, 0, 0}, {"xchg", M_XCHG, 0, 0}, {"test", M_TEST, 0, 0}, {"lea", M_LEA, 0, 0},
    {"lds", M_LDS, 0xC5, 0}, {"les", M_LDS, 0xC4, 0},
    {"inc", M_INC, 0, 0}, {"dec", M_DEC, 1, 0},
    {"not", M_NOT, 2, 0}, {"neg", M_NEG, 3, 0}, {"mul", M_MUL, 4, 0}, {"imul", M_IMUL, 5, 0},
    {"div", M_DIV, 6, 0}, {"idiv", M_IDIV, 7, 0},
    {"rol", M_ROL, 0, 0}, {"ror", M_ROR, 1, 0}, {"rcl", M_RCL, 2, 0}, {"rcr", M_RCR, 3, 0},
    {"shl", M_SHL, 4, 0}, {"sal", M_SHL, 4, 0}, {"shr", M_SHR, 5, 0}, {"sar", M_SAR, 7, 0},
    {"push", M_PUSH, 0, 0}, {"pop", M_POP, 0, 0},
    {"jmp", M_JMP, 0, 0}, {"call", M_CALL, 0, 0}, {"ret", M_RET, 0, 0}, {"retn", M_RET, 0, 0},
    {"retf", M_RETF, 0, 0}, {"iret", M_IRET, 0, 0}, {"int", M_INT, 0, 0}, {"int3", M_INT3, 0, 0},
    {"jo", M_JCC, 0, 0}, {"jno", M_JCC, 1, 0},
    {"jb", M_JCC, 2, 0}, {"jc", M_JCC, 2, 0}, {"jnae", M_JCC, 2, 0},
    {"jae", M_JCC, 3, 0}, {"jnb", M_JCC, 3, 0}, {"jnc", M_JCC, 3, 0},
    {"je", M_JCC, 4, 0}, {"jz", M_JCC, 4, 0},
    {"jne", M_JCC, 5, 0}, {"jnz", M_JCC, 5, 0},
    {"jbe", M_JCC, 6, 0}, {"jna", M_JCC, 6, 0},
    {"ja", M_JCC, 7, 0}, {"jnbe", M_JCC, 7, 0},
    {"js", M_JCC, 8, 0}, {"jns", M_JCC, 9, 0},
    {"jp", M_JCC, 10, 0}, {"jpe", M_JCC, 10, 0},
    {"jnp", M_JCC, 11, 0}, {"jpo", M_JCC, 11, 0},
    {"jl", M_JCC, 12, 0}, {"jnge", M_JCC, 12, 0},
    {"jge", M_JCC, 13, 0}, {"jnl", M_JCC, 13, 0},
    {"jle", M_JCC, 14, 0}, {"jng", M_JCC, 14, 0},
    {"jg", M_JCC, 15, 0}, {"jnle", M_JCC, 15, 0},
    {"loop", M_LOOP, 0, 0}, {"loope", M_LOOPE, 0, 0}, {"loopz", M_LOOPE, 0, 0},
    {"loopne", M_LOOPNE, 0, 0}, {"loopnz", M_LOOPNE, 0, 0}, {"jcxz", M_JCXZ, 0, 0},
    {"in", M_IN, 0, 0}, {"out", M_OUT, 0, 0},
    {"movsb", M_SIMPLE, 0xA4, 1}, {"movsw", M_SIMPLE, 0xA5, 1},
    {"cmpsb", M_SIMPLE, 0xA6, 1}, {"cmpsw", M_SIMPLE, 0xA7, 1},
    {"stosb", M_SIMPLE, 0xAA, 1}, {"stosw", M_SIMPLE, 0xAB, 1},
    {"lodsb", M_SIMPLE, 0xAC, 1}, {"lodsw", M_SIMPLE, 0xAD, 1},
    {"scasb", M_SIMPLE, 0xAE, 1}, {"scasw", M_SIMPLE, 0xAF, 1},
    {"nop", M_SIMPLE, 0x90, 0}, {"hlt", M_SIMPLE, 0xF4, 0},
    {"cbw", M_SIMPLE, 0x98, 0}, {"cwd", M_SIMPLE, 0x99, 0},
    {"pushf", M_SIMPLE, 0x9C, 0}, {"popf", M_SIMPLE, 0x9D, 0},
    {"sahf", M_SIMPLE, 0x9E, 0}, {"lahf", M_SIMPLE, 0x9F, 0},
    {"clc", M_SIMPLE, 0xF8, 0}, {"stc", M_SIMPLE, 0xF9, 0}, {"cmc", M_SIMPLE, 0xF5, 0},
    {"cli", M_SIMPLE, 0xFA, 0}, {"sti", M_SIMPLE, 0xFB, 0},
    {"cld", M_SIMPLE, 0xFC, 0}, {"std", M_SIMPLE, 0xFD, 0},
    {NULL, 0, 0, 0}
};

/* Real 8086 instructions this machine does not support (clear error). */
static const char *UNSUPPORTED_8086[] = {
    "daa", "das", "aaa", "aas", "aam", "aad", "xlat", "xlatb",
    "lock", "wait", "fwait", "esc", "into", "salc", NULL
};

/* Instructions from later CPUs (error explains why). */
static const char *LATER_CPUS[] = {
    "pusha", "popa", "pushaw", "popaw", "enter", "leave", "bound",
    "ins", "insb", "insw", "outs", "outsb", "outsw",
    "movsx", "movzx", "pushad", "popad", "cwde", "cdq", "lfs", "lgs", "lss",
    "bt", "bts", "btr", "btc", "bsf", "bsr", "shld", "shrd",
    "seto", "setno", "setb", "setc", "setnc", "setae", "setz", "sete", "setnz", "setne",
    "jecxz", "movsd", "stosd", "lodsd", "scasd", "cmpsd", "pushfd", "popfd", "iretd",
    "cmov", "cmove", "cmovne", "cmovz", "cmovnz", "bswap", "xadd", "cmpxchg", "cpuid",
    "rdtsc", "arpl", "sldt", "str", "lgdt", "lidt", "smsw", "lmsw", "clts", "syscall", NULL
};

enum { R_NONE = 0, R_8, R_16, R_SEG };

typedef struct { const char *name; int kind; int num; } reg_t;

static const reg_t REGS[] = {
    {"al", R_8, 0}, {"cl", R_8, 1}, {"dl", R_8, 2}, {"bl", R_8, 3},
    {"ah", R_8, 4}, {"ch", R_8, 5}, {"dh", R_8, 6}, {"bh", R_8, 7},
    {"ax", R_16, 0}, {"cx", R_16, 1}, {"dx", R_16, 2}, {"bx", R_16, 3},
    {"sp", R_16, 4}, {"bp", R_16, 5}, {"si", R_16, 6}, {"di", R_16, 7},
    {"es", R_SEG, 0}, {"cs", R_SEG, 1}, {"ss", R_SEG, 2}, {"ds", R_SEG, 3},
    {NULL, 0, 0}
};

static const char *REGS_32[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "fs", "gs", NULL
};

static int tok_ieq(const tok_t *t, const char *w)
{
    int i;
    if (t->type != TK_ID) return 0;
    for (i = 0; i < t->len; i++) {
        if (!w[i] || tolower((unsigned char)t->s[i]) != w[i]) return 0;
    }
    return w[i] == 0;
}

static const mnem_t *find_mnem(const tok_t *t)
{
    int i;
    for (i = 0; MNEMS[i].name; i++) if (tok_ieq(t, MNEMS[i].name)) return &MNEMS[i];
    return NULL;
}

static int in_list(const tok_t *t, const char **list)
{
    int i;
    for (i = 0; list[i]; i++) if (tok_ieq(t, list[i])) return 1;
    return 0;
}

static const reg_t *find_reg(const tok_t *t)
{
    int i;
    for (i = 0; REGS[i].name; i++) if (tok_ieq(t, REGS[i].name)) return &REGS[i];
    return NULL;
}

enum { D_NONE, D_DB, D_DW, D_RESB, D_RESW, D_EQU, D_TIMES, D_ORG, D_BITS, D_CPU,
       D_REP, D_REPNE, D_OTHER_DATA, D_SECTION };

static int find_directive(const tok_t *t)
{
    if (tok_ieq(t, "db")) return D_DB;
    if (tok_ieq(t, "dw")) return D_DW;
    if (tok_ieq(t, "resb")) return D_RESB;
    if (tok_ieq(t, "resw")) return D_RESW;
    if (tok_ieq(t, "equ")) return D_EQU;
    if (tok_ieq(t, "times")) return D_TIMES;
    if (tok_ieq(t, "org")) return D_ORG;
    if (tok_ieq(t, "bits")) return D_BITS;
    if (tok_ieq(t, "cpu")) return D_CPU;
    if (tok_ieq(t, "rep") || tok_ieq(t, "repe") || tok_ieq(t, "repz")) return D_REP;
    if (tok_ieq(t, "repne") || tok_ieq(t, "repnz")) return D_REPNE;
    if (tok_ieq(t, "dd") || tok_ieq(t, "dq") || tok_ieq(t, "dt") || tok_ieq(t, "resd") ||
        tok_ieq(t, "resq")) return D_OTHER_DATA;
    if (tok_ieq(t, "section") || tok_ieq(t, "segment")) return D_SECTION;
    return D_NONE;
}

static int is_keyword(const tok_t *t)
{
    return find_mnem(t) || find_directive(t) != D_NONE || in_list(t, UNSUPPORTED_8086) ||
           in_list(t, LATER_CPUS);
}

/* ====================================================================== */
/* Assembler data structures                                              */
/* ====================================================================== */

enum { K_EMPTY, K_INSTR, K_DB, K_DW, K_RESB, K_RESW, K_EQU, K_ORG, K_IGNORE, K_ERROR, K_SECTION };

enum { SEC_TEXT, SEC_DATA, SEC_BSS, SEC_COUNT };
static const char *SECTION_NAMES[SEC_COUNT] = { ".text", ".data", ".bss" };

#define MAX_OPS 3

typedef struct {
    int line;
    int label;                  /* token index or -1 */
    int kind;
    int times_beg, times_end;   /* -1 if there is no TIMES */
    const mnem_t *mnem;
    uint8_t rep;                /* 0, 0xF2, 0xF3 */
    int args_beg, args_end;     /* rest of the line */
    int nops;
    int op_beg[MAX_OPS], op_end[MAX_OPS];
    uint16_t addr, size;
    uint8_t sticky;             /* bits: 1 long jump, 2/4 long displacement, 8 long immediate */
    uint8_t section;            /* K_SECTION: the section it switches to */
} stmt_t;

typedef struct {
    char *name;
    int len;
    int64_t value;
    int reloc;                  /* 1 = depends on an address */
    int is_label;
    int def_pass;               /* last pass in which it was defined */
    int ever_defined;
    int line;
} sym_t;

typedef struct {
    int64_t v;
    int reloc;
    int unknown;
} val_t;

typedef struct {
    const char *src;
    tok_t *toks; int ntok, captok;
    stmt_t *st; int nst, capst;
    sym_t *syms; int nsym, capsym;

    int pass, final, changed;
    int64_t org; int org_set;
    int64_t addr;               /* current address (advances on each TIMES repetition) */
    int64_t line_start;         /* value of $: start of the line, as in NASM */
    int cur_line;

    int last_global;            /* token of the last non-local label, or -1 */

    uint8_t *out; size_t outlen;  /* the last pass: 64 KB, bytes at address - org */
    int64_t wpos;               /* where out_bytes writes (an address) */
    int sec;                    /* the current section */
    int64_t sec_base[SEC_COUNT], sec_addr[SEC_COUNT], sec_size[SEC_COUNT];
    asm86_line_t *lines; int nlines, caplines;
    asm86_result_t *res;
    int stmt_failed;            /* the current statement had an error */
} asm_t;

/* Makes the text valid UTF-8: every invalid sequence (raw bytes from the
 * source, or a character cut off by truncation) is replaced with '?'. */
static void utf8_sanitize(char *m)
{
    unsigned char *p = (unsigned char *)m;
    while (*p) {
        int n = 0, i;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0 && *p >= 0xC2) n = 1;
        else if ((*p & 0xF0) == 0xE0) n = 2;
        else if ((*p & 0xF8) == 0xF0 && *p <= 0xF4) n = 3;
        for (i = 1; n && i <= n; i++)
            if ((p[i] & 0xC0) != 0x80) n = 0;
        if (!n) { *p++ = '?'; continue; }
        p += n + 1;
    }
}

static void add_error(asm_t *A, int line, int force, const char *fmt, ...)
{
    va_list ap;
    asm86_result_t *r = A->res;
    A->stmt_failed = 1;
    if (!A->final && !force) return;
    if (r->error_count >= ASM86_MAX_ERRORS) return;
    /* do not repeat errors for the same line */
    if (r->error_count > 0 && r->errors[r->error_count - 1].line == line && !force) return;
    r->errors[r->error_count].line = line;
    va_start(ap, fmt);
    vsnprintf(r->errors[r->error_count].message, sizeof(r->errors[0].message), fmt, ap);
    va_end(ap);
    utf8_sanitize(r->errors[r->error_count].message);
    r->error_count++;
}

static void *grow(void *p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return p;
    while (*cap < need) *cap = *cap ? *cap * 2 : 64;
    return realloc(p, (size_t)*cap * elem);
}

/* ====================================================================== */
/* Lexer                                                                   */
/* ====================================================================== */

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 99;
}

static int parse_digits(const char *s, int n, int base, int64_t *out)
{
    int64_t v = 0;
    int i, any = 0;
    for (i = 0; i < n; i++) {
        int d;
        if (s[i] == '_') continue;
        d = digit_val((unsigned char)s[i]);
        if (d >= base) return 0;
        v = v * base + d;
        any = 1;
    }
    *out = v;
    return any;
}

/* Parses a NASM-style number. s points to the text, n is its length. */
static int parse_number(const char *s, int n, int64_t *out)
{
    char suf;
    if (n > 2 && s[0] == '0') {
        int base = 0;
        switch (tolower((unsigned char)s[1])) {
        case 'x': case 'h': base = 16; break;
        case 'b': case 'y': base = 2; break;
        case 'o': case 'q': base = 8; break;
        case 'd': case 't': base = 10; break;
        }
        if (base && parse_digits(s + 2, n - 2, base, out)) return 1;
    }
    suf = (char)tolower((unsigned char)s[n - 1]);
    if (n > 1) {
        switch (suf) {
        case 'h': case 'x': return parse_digits(s, n - 1, 16, out);
        case 'b': case 'y': return parse_digits(s, n - 1, 2, out);
        case 'o': case 'q': return parse_digits(s, n - 1, 8, out);
        case 'd': case 't': return parse_digits(s, n - 1, 10, out);
        }
    }
    return parse_digits(s, n, 10, out);
}

/* Like NASM, bytes >= 80h (accented letters in UTF-8 or any code page) are
 * valid in labels. */
static int is_id_start(int c) { return isalpha(c) || c == '_' || c == '.' || c == '?' || c == '@' || c >= 0x80; }
static int is_id_char(int c) { return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c == '#' || c == '~' || c >= 0x80; }

static void push_tok(asm_t *A, tok_t t)
{
    A->toks = (tok_t *)grow(A->toks, &A->captok, A->ntok + 1, sizeof(tok_t));
    A->toks[A->ntok++] = t;
}

/* Tokenizes the line [p, e). Returns 0 on a lexical error. */
static int lex_line(asm_t *A, const char *p, const char *e, int line)
{
    while (p < e) {
        tok_t t;
        unsigned char c = (unsigned char)*p;
        memset(&t, 0, sizeof(t));
        if (c == ';') break;
        if (isspace(c)) { p++; continue; }
        t.s = p;
        if (c == '$' && p + 1 < e && p[1] == '$') {
            t.type = TK_DOLLAR2; t.len = 2; p += 2;
        } else if (c == '$' && p + 1 < e && isdigit((unsigned char)p[1])) {
            const char *q = p + 1;
            while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
            t.type = TK_NUM; t.len = (int)(q - p);
            if (!parse_digits(p + 1, (int)(q - p - 1), 16, &t.num)) {
                add_error(A, line, 1, "invalid hexadecimal number: %.*s", t.len, p);
                return 0;
            }
            p = q;
        } else if (c == '$') {
            t.type = TK_DOLLAR; t.len = 1; p++;
        } else if (isdigit(c)) {
            const char *q = p;
            while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
            t.type = TK_NUM; t.len = (int)(q - p);
            if (!parse_number(p, t.len, &t.num)) {
                add_error(A, line, 1, "invalid number: %.*s", t.len, p);
                return 0;
            }
            p = q;
        } else if (is_id_start(c)) {
            const char *q = p + 1;
            while (q < e && is_id_char((unsigned char)*q)) q++;
            t.type = TK_ID; t.len = (int)(q - p);
            p = q;
        } else if (c == '\'' || c == '"') {
            const char *q = p + 1;
            while (q < e && *q != (char)c) q++;
            if (q >= e) {
                add_error(A, line, 1, "unterminated string (missing %c)", c);
                return 0;
            }
            t.type = TK_STR; t.s = p + 1; t.len = (int)(q - p - 1);
            p = q + 1;
        } else if (c == '`') {
            add_error(A, line, 1, "backquote strings are not supported; use single or double quotes");
            return 0;
        } else {
            t.type = TK_PUNCT; t.len = 1; t.punct = (char)c;
            if (p + 1 < e) {
                if (c == '<' && p[1] == '<') { t.punct = P_SHL; t.len = 2; }
                else if (c == '>' && p[1] == '>') { t.punct = P_SHR; t.len = 2; }
                else if (c == '/' && p[1] == '/') { t.punct = P_SDIV; t.len = 2; }
                else if (c == '%' && p[1] == '%') { t.punct = P_SMOD; t.len = 2; }
            }
            if (!strchr(",[]:()+-*/%|^&~!=", c) && t.len == 1) {
                add_error(A, line, 1, "unexpected character: '%c'", c);
                return 0;
            }
            p += t.len;
        }
        push_tok(A, t);
    }
    {
        tok_t t;
        memset(&t, 0, sizeof(t));
        t.type = TK_EOL;
        t.s = e;
        push_tok(A, t);
    }
    return 1;
}

/* ====================================================================== */
/* Symbols                                                                 */
/* ====================================================================== */

/* Builds the full name (resolving .local labels). */
static int full_name(asm_t *A, const tok_t *t, char *buf, int cap)
{
    int n = 0;
    if (t->len > 0 && t->s[0] == '.' && !(t->len > 1 && t->s[1] == '.')) {
        if (A->last_global >= 0) {
            const tok_t *g = &A->toks[A->last_global];
            if (g->len >= cap) return -1;
            memcpy(buf, g->s, (size_t)g->len);
            n = g->len;
        }
    }
    if (n + t->len >= cap) return -1;
    memcpy(buf + n, t->s, (size_t)t->len);
    n += t->len;
    buf[n] = 0;
    return n;
}

static sym_t *find_sym(asm_t *A, const char *name, int len)
{
    int i;
    for (i = 0; i < A->nsym; i++)
        if (A->syms[i].len == len && memcmp(A->syms[i].name, name, (size_t)len) == 0) return &A->syms[i];
    return NULL;
}

static void define_sym(asm_t *A, const tok_t *t, val_t v, int is_label, int line)
{
    char buf[256];
    int n = full_name(A, t, buf, sizeof(buf));
    sym_t *s;
    if (n < 0) { add_error(A, line, 1, "label name too long"); return; }
    if (find_reg(t) || is_keyword(t)) {
        add_error(A, line, 0, "'%.*s' is a reserved word and cannot be used as a label", t->len, t->s);
        return;
    }
    s = find_sym(A, buf, n);
    if (!s) {
        A->syms = (sym_t *)grow(A->syms, &A->capsym, A->nsym + 1, sizeof(sym_t));
        s = &A->syms[A->nsym++];
        memset(s, 0, sizeof(*s));
        s->name = (char *)malloc((size_t)n + 1);
        memcpy(s->name, buf, (size_t)n + 1);
        s->len = n;
        s->def_pass = -1;
        A->changed = 1;
    }
    if (s->def_pass == A->pass) {
        add_error(A, line, 0, "'%s' is already defined on line %d", buf, s->line);
        return;
    }
    if (s->value != v.v || s->reloc != v.reloc || !s->ever_defined || v.unknown) A->changed = 1;
    s->value = v.v;
    s->reloc = v.reloc;
    s->is_label = is_label;
    s->def_pass = A->pass;
    s->ever_defined = !v.unknown;
    s->line = line;
}

/* ====================================================================== */
/* Expressions                                                             */
/* ====================================================================== */

typedef struct {
    asm_t *A;
    int pos, end;
    int line;
    int err;
} ex_t;

static val_t ex_or(ex_t *x);

static val_t vnum(int64_t v) { val_t r; r.v = v; r.reloc = 0; r.unknown = 0; return r; }

static int ex_punct(ex_t *x, char p)
{
    const tok_t *t = &x->A->toks[x->pos];
    if (x->pos < x->end && t->type == TK_PUNCT && t->punct == p) { x->pos++; return 1; }
    return 0;
}

static void ex_fail(ex_t *x, const char *msg, const tok_t *t)
{
    if (!x->err) {
        if (t && t->type != TK_EOL && t->len)
            add_error(x->A, x->line, 0, "%s: '%.*s'", msg, t->len, t->s);
        else
            add_error(x->A, x->line, 0, "%s", msg);
    }
    x->err = 1;
}

static val_t ex_primary(ex_t *x)
{
    asm_t *A = x->A;
    const tok_t *t;
    val_t r = vnum(0);

    if (x->pos >= x->end) { ex_fail(x, "missing value in expression", NULL); return r; }
    t = &A->toks[x->pos];
    switch (t->type) {
    case TK_NUM: x->pos++; return vnum(t->num);
    case TK_STR: {
        int i;
        if (t->len == 0 || t->len > 4) { ex_fail(x, "invalid character constant", t); return r; }
        for (i = 0; i < t->len; i++) r.v |= (int64_t)(unsigned char)t->s[i] << (8 * i);
        x->pos++;
        return r;
    }
    case TK_DOLLAR: x->pos++; r.v = A->line_start; r.reloc = 1; return r;
    case TK_DOLLAR2: x->pos++; r.v = A->sec_base[A->sec]; r.reloc = 1; return r;
    case TK_PUNCT:
        if (t->punct == '(') {
            x->pos++;
            r = ex_or(x);
            if (!ex_punct(x, ')')) ex_fail(x, "missing ')'", &A->toks[x->pos]);
            return r;
        }
        ex_fail(x, "expected a value", t);
        return r;
    case TK_ID: {
        char buf[256];
        int n;
        sym_t *s;
        if (find_reg(t)) { ex_fail(x, "a register cannot be used here", t); return r; }
        if (in_list(t, REGS_32)) { ex_fail(x, "32-bit register: the 8086 only has 8- and 16-bit registers", t); return r; }
        if (is_keyword(t)) { ex_fail(x, "expected a value", t); return r; }
        n = full_name(A, t, buf, sizeof(buf));
        x->pos++;
        s = (n >= 0) ? find_sym(A, buf, n) : NULL;
        if (!s || !s->ever_defined) {
            if (A->final) {
                add_error(A, x->line, 0, "undefined symbol: '%s'", n >= 0 ? buf : "?");
                x->err = 1;
            }
            r.unknown = 1;
            r.reloc = 0;
            return r;
        }
        r.v = s->value;
        r.reloc = s->reloc;
        return r;
    }
    default:
        ex_fail(x, "expected a value", t);
        return r;
    }
}

static val_t ex_unary(ex_t *x)
{
    val_t r;
    if (ex_punct(x, '-')) { r = ex_unary(x); r.v = -r.v; r.reloc = -r.reloc; return r; }
    if (ex_punct(x, '+')) return ex_unary(x);
    if (ex_punct(x, '~')) { r = ex_unary(x); r.v = ~r.v; return r; }
    if (ex_punct(x, '!')) { r = ex_unary(x); r.v = !r.v; return r; }
    return ex_primary(x);
}

static val_t combine(val_t a, val_t b, int64_t v)
{
    val_t r;
    r.v = v;
    r.reloc = (a.reloc || b.reloc) ? 1 : 0;
    r.unknown = a.unknown || b.unknown;
    return r;
}

static val_t ex_mul(ex_t *x)
{
    val_t a = ex_unary(x);
    for (;;) {
        char op;
        val_t b;
        const tok_t *t = &x->A->toks[x->pos];
        if (x->pos >= x->end || t->type != TK_PUNCT) return a;
        op = t->punct;
        if (op != '*' && op != '/' && op != '%' && op != P_SDIV && op != P_SMOD) return a;
        x->pos++;
        b = ex_unary(x);
        if (op == '*') { a = combine(a, b, a.v * b.v); continue; }
        if (b.v == 0) {
            if (!b.unknown) ex_fail(x, "division by zero", NULL);
            a = combine(a, b, 0);
            continue;
        }
        switch (op) {
        case '/': a = combine(a, b, (int64_t)((uint64_t)a.v / (uint64_t)b.v)); break;
        case '%': a = combine(a, b, (int64_t)((uint64_t)a.v % (uint64_t)b.v)); break;
        case P_SDIV: a = combine(a, b, a.v / b.v); break;
        default: a = combine(a, b, a.v % b.v); break;
        }
    }
}

static val_t ex_add(ex_t *x)
{
    val_t a = ex_mul(x);
    for (;;) {
        val_t b, r;
        if (ex_punct(x, '+')) {
            b = ex_mul(x);
            r = combine(a, b, a.v + b.v);
            r.reloc = a.reloc + b.reloc;
            a = r;
        } else if (ex_punct(x, '-')) {
            b = ex_mul(x);
            r = combine(a, b, a.v - b.v);
            r.reloc = a.reloc - b.reloc;
            a = r;
        } else {
            return a;
        }
    }
}

static val_t ex_shift(ex_t *x)
{
    val_t a = ex_add(x);
    for (;;) {
        val_t b;
        if (ex_punct(x, P_SHL)) { b = ex_add(x); a = combine(a, b, (int64_t)((uint64_t)a.v << (b.v & 63))); }
        else if (ex_punct(x, P_SHR)) { b = ex_add(x); a = combine(a, b, (int64_t)((uint64_t)a.v >> (b.v & 63))); }
        else return a;
    }
}

static val_t ex_and(ex_t *x)
{
    val_t a = ex_shift(x);
    while (ex_punct(x, '&')) { val_t b = ex_shift(x); a = combine(a, b, a.v & b.v); }
    return a;
}

static val_t ex_xor(ex_t *x)
{
    val_t a = ex_and(x);
    while (ex_punct(x, '^')) { val_t b = ex_and(x); a = combine(a, b, a.v ^ b.v); }
    return a;
}

static val_t ex_or(ex_t *x)
{
    val_t a = ex_xor(x);
    while (ex_punct(x, '|')) { val_t b = ex_xor(x); a = combine(a, b, a.v | b.v); }
    return a;
}

/* Evaluates an expression that must span exactly [beg, end). */
static val_t eval_range(asm_t *A, int beg, int end, int line, int *err)
{
    ex_t x;
    val_t v;
    x.A = A; x.pos = beg; x.end = end; x.line = line; x.err = 0;
    v = ex_or(&x);
    if (!x.err && x.pos != end) ex_fail(&x, "unexpected text after expression", &A->toks[x.pos]);
    if (err) *err = x.err;
    return v;
}

/* ====================================================================== */
/* Operands                                                                */
/* ====================================================================== */

enum { O_NONE, O_REG8, O_REG16, O_SREG, O_IMM, O_MEM };
enum { J_DEFAULT, J_SHORT, J_NEAR, J_FAR };

typedef struct {
    int type;
    int reg;
    int size;          /* 0 unspecified, 1 byte, 2 word */
    int imm_size;      /* size keyword given on the immediate (byte/word) */
    int jmod;
    val_t imm;         /* the offset, for segment:offset */
    int far_imm;       /* immediate written as segment:offset */
    val_t far_seg;
    /* memory */
    int seg;           /* -1 or 0..3 */
    int base;          /* -1, 3 (bx), 5 (bp) */
    int index;         /* -1, 6 (si), 7 (di) */
    val_t disp;
    int has_disp;
    int tok;           /* first token, for messages */
} opnd_t;

/* Can this token end an operand? (to tell binary +/- from unary) */
static int ends_value(const tok_t *t)
{
    return t->type == TK_NUM || t->type == TK_ID || t->type == TK_STR || t->type == TK_DOLLAR ||
           t->type == TK_DOLLAR2 || (t->type == TK_PUNCT && t->punct == ')');
}

static int parse_mem(asm_t *A, int beg, int end, int line, opnd_t *o)
{
    int i = beg;
    o->type = O_MEM;
    o->seg = -1; o->base = -1; o->index = -1;
    o->disp = vnum(0); o->has_disp = 0;

    if (end - beg >= 2 && A->toks[i].type == TK_ID && A->toks[i + 1].type == TK_PUNCT && A->toks[i + 1].punct == ':') {
        const reg_t *r = find_reg(&A->toks[i]);
        if (!r || r->kind != R_SEG) {
            add_error(A, line, 0, "'%.*s' is not a segment register", A->toks[i].len, A->toks[i].s);
            return 0;
        }
        o->seg = r->num;
        i += 2;
    }
    if (i >= end) { add_error(A, line, 0, "empty memory address"); return 0; }

    while (i < end) {
        int sign = 1, tb, te, depth = 0;
        /* leading signs of the term */
        while (i < end && A->toks[i].type == TK_PUNCT && (A->toks[i].punct == '+' || A->toks[i].punct == '-')) {
            if (A->toks[i].punct == '-') sign = -sign;
            i++;
        }
        tb = i;
        while (i < end) {
            const tok_t *t = &A->toks[i];
            if (t->type == TK_PUNCT && t->punct == '(') depth++;
            if (t->type == TK_PUNCT && t->punct == ')') depth--;
            if (depth == 0 && t->type == TK_PUNCT && (t->punct == '+' || t->punct == '-') && i > tb &&
                ends_value(&A->toks[i - 1])) break;
            i++;
        }
        te = i;
        if (tb == te) { add_error(A, line, 0, "missing term in address"); return 0; }
        if (te - tb == 1 && A->toks[tb].type == TK_ID && find_reg(&A->toks[tb])) {
            const reg_t *r = find_reg(&A->toks[tb]);
            if (r->kind != R_16 || (r->num != 3 && r->num != 5 && r->num != 6 && r->num != 7)) {
                add_error(A, line, 0, "the 8086 can only address memory through BX, BP, SI and DI (not %s)", r->name);
                return 0;
            }
            if (sign < 0) { add_error(A, line, 0, "a register cannot be subtracted in an address"); return 0; }
            if (r->num == 3 || r->num == 5) {
                if (o->base >= 0) { add_error(A, line, 0, "invalid register combination on the 8086 (only BX/BP + SI/DI)"); return 0; }
                o->base = r->num;
            } else {
                if (o->index >= 0) { add_error(A, line, 0, "invalid register combination on the 8086 (only BX/BP + SI/DI)"); return 0; }
                o->index = r->num;
            }
        } else {
            int err;
            val_t v = eval_range(A, tb, te, line, &err);
            if (err) return 0;
            if (sign < 0) { v.v = -v.v; v.reloc = -v.reloc; }
            o->disp.v += v.v;
            o->disp.reloc += v.reloc;
            o->disp.unknown |= v.unknown;
            o->has_disp = 1;
        }
    }
    return 1;
}

static int parse_operand(asm_t *A, int beg, int end, int line, opnd_t *o)
{
    const tok_t *t;
    memset(o, 0, sizeof(*o));
    o->tok = beg;
    o->seg = -1; o->base = -1; o->index = -1;
    if (beg >= end) { add_error(A, line, 0, "missing operand"); return 0; }

    for (;;) {
        t = &A->toks[beg];
        if (beg >= end || t->type != TK_ID) break;
        if (tok_ieq(t, "byte")) { o->size = 1; beg++; }
        else if (tok_ieq(t, "word")) { o->size = 2; beg++; }
        else if (tok_ieq(t, "short")) { o->jmod = J_SHORT; beg++; }
        else if (tok_ieq(t, "near")) { o->jmod = J_NEAR; beg++; }
        else if (tok_ieq(t, "strict")) { beg++; }
        else if (tok_ieq(t, "far")) { o->jmod = J_FAR; beg++; }
        else if (tok_ieq(t, "dword") || tok_ieq(t, "qword") || tok_ieq(t, "tword")) {
            add_error(A, line, 0, "the 8086 only has BYTE and WORD operands"); return 0;
        } else break;
    }
    if (beg >= end) { add_error(A, line, 0, "missing operand"); return 0; }
    for (t = &A->toks[beg]; t < &A->toks[end]; t++) {
        if (tok_ieq(t, "ptr")) {
            add_error(A, line, 0, "PTR is MASM syntax; in NASM write e.g. 'word [bx]'");
            return 0;
        }
    }
    t = &A->toks[beg];

    if (t->type == TK_PUNCT && t->punct == '[') {
        const tok_t *last = &A->toks[end - 1];
        if (!(last->type == TK_PUNCT && last->punct == ']')) {
            add_error(A, line, 0, "missing ']' or unexpected text after ']'");
            return 0;
        }
        return parse_mem(A, beg + 1, end - 1, line, o);
    }
    if (end - beg == 1 && t->type == TK_ID) {
        const reg_t *r = find_reg(t);
        if (r) {
            o->type = r->kind == R_8 ? O_REG8 : r->kind == R_16 ? O_REG16 : O_SREG;
            o->reg = r->num;
            if (o->size && ((o->size == 1) != (r->kind == R_8))) {
                add_error(A, line, 0, "the size given does not match register %s", r->name);
                return 0;
            }
            if (r->kind != R_8) o->size = 2; else o->size = 1;
            return 1;
        }
    }
    if (end - beg >= 2 && t->type == TK_ID && find_reg(t) && A->toks[beg + 1].type == TK_PUNCT &&
        A->toks[beg + 1].punct == ':') {
        add_error(A, line, 0, "the segment goes inside the brackets: [%.*s:...]", t->len, t->s);
        return 0;
    }
    {
        int err, i, depth = 0;
        o->type = O_IMM;
        o->imm_size = o->size;
        o->size = 0;
        /* segment:offset (far JMP/CALL) */
        for (i = beg; i < end; i++) {
            t = &A->toks[i];
            if (t->type == TK_PUNCT && t->punct == '(') depth++;
            if (t->type == TK_PUNCT && t->punct == ')') depth--;
            if (depth == 0 && t->type == TK_PUNCT && t->punct == ':') {
                o->far_imm = 1;
                o->far_seg = eval_range(A, beg, i, line, &err);
                if (err) return 0;
                beg = i + 1;
                if (beg >= end) { add_error(A, line, 0, "missing offset after ':'"); return 0; }
                break;
            }
        }
        o->imm = eval_range(A, beg, end, line, &err);
        return !err;
    }
}

/* ====================================================================== */
/* Encoding                                                                */
/* ====================================================================== */

typedef struct {
    uint8_t b[16];
    int n;
} enc_t;

static void eb(enc_t *e, int v) { if (e->n < 16) e->b[e->n++] = (uint8_t)v; }
static void ew(enc_t *e, int64_t v) { eb(e, (int)(v & 0xFF)); eb(e, (int)((v >> 8) & 0xFF)); }

static int fits_s8(int64_t v) { return v >= -128 && v <= 127; }

static int check_byte(asm_t *A, int line, val_t v)
{
    if (!v.unknown && (v.v < -128 || v.v > 255)) {
        add_error(A, line, 0, "value %lld does not fit in a byte (-128..255)", (long long)v.v);
        return 0;
    }
    if (v.reloc) { add_error(A, line, 0, "an address does not fit in a byte"); return 0; }
    return 1;
}

static int check_word(asm_t *A, int line, val_t v)
{
    if (!v.unknown && (v.v < -32768 || v.v > 65535)) {
        add_error(A, line, 0, "value %lld does not fit in a word (-32768..65535)", (long long)v.v);
        return 0;
    }
    return 1;
}

/* 16-bit immediate that can be encoded as a sign-extended byte */
static int fits_simm8_word(val_t v)
{
    int64_t w;
    if (v.reloc) return 0;
    if (v.unknown) return 1; /* optimistic: corrected in a later pass */
    w = v.v & 0xFFFF;
    return w <= 0x7F || w >= 0xFF80;
}

/* Segment override prefix, if the operand has one */
static void seg_prefix(enc_t *e, const opnd_t *o)
{
    static const int pfx[4] = {0x26, 0x2E, 0x36, 0x3E};
    if (o->type == O_MEM && o->seg >= 0) eb(e, pfx[o->seg]);
}

/* Emits ModRM (+displacement) for operand E with the given reg field. */
static void modrm(asm_t *A, stmt_t *s, enc_t *e, int reg, const opnd_t *o, int which)
{
    int rm, mod;
    uint8_t bit = (uint8_t)(which ? 4 : 2);

    if (o->type == O_REG8 || o->type == O_REG16 || o->type == O_SREG) {
        eb(e, 0xC0 | (reg << 3) | o->reg);
        return;
    }
    if (o->base < 0 && o->index < 0) {
        eb(e, (reg << 3) | 6);
        ew(e, o->disp.v);
        return;
    }
    if (o->base == 3) rm = o->index == 6 ? 0 : o->index == 7 ? 1 : 7;
    else if (o->base == 5) rm = o->index == 6 ? 2 : o->index == 7 ? 3 : 6;
    else rm = o->index == 6 ? 4 : 5;

    {
        /* displacements are 16-bit arithmetic: 0FFFFh is the same as -1 */
        int64_t d = o->disp.v & 0xFFFF;
        if (d >= 0x8000) d -= 0x10000;
        if (o->disp.reloc || (s->sticky & bit)) mod = 2;
        else if (o->disp.unknown) mod = 1;
        else if (d == 0 && rm != 6) mod = 0;
        else if (fits_s8(d)) mod = 1;
        else mod = 2;
    }
    if (mod == 2 && A->pass >= STICKY_PASS) s->sticky |= bit;

    eb(e, (mod << 6) | (reg << 3) | rm);
    if (mod == 1) eb(e, (int)(o->disp.v & 0xFF));
    else if (mod == 2) ew(e, o->disp.v);
}

static int is_reg(const opnd_t *o) { return o->type == O_REG8 || o->type == O_REG16; }
static int is_rm(const opnd_t *o) { return is_reg(o) || o->type == O_MEM; }

/* Common size of two operands (1 byte, 2 word). 0 = error already reported. */
static int op_size2(asm_t *A, int line, const opnd_t *a, const opnd_t *b)
{
    int sa = a->size, sb = (b && b->type != O_IMM) ? b->size : 0;
    if (!sa && b && b->type == O_IMM && b->imm_size) sa = b->imm_size; /* mov [bx], word 5 */
    if (sa && sb && sa != sb) { add_error(A, line, 0, "operand sizes do not match"); return 0; }
    if (!sa) sa = sb;
    if (!sa) { add_error(A, line, 0, "operation size not specified: use 'byte [...]' or 'word [...]'"); return 0; }
    return sa;
}

static void encode_rel8(asm_t *A, stmt_t *s, enc_t *e, int opcode, const opnd_t *t, const char *what,
                        const char *hint)
{
    int64_t rel = t->imm.v - (A->addr + e->n + 2);
    eb(e, opcode);
    if (!t->imm.unknown && !fits_s8(rel)) {
        add_error(A, s->line, 0, "%s out of range: target is %lld bytes away and the limit is -128..127%s",
                  what, (long long)rel, hint);
    }
    eb(e, (int)(rel & 0xFF));
}

static int encode_instr(asm_t *A, stmt_t *s, enc_t *e)
{
    opnd_t o[MAX_OPS];
    int i, n = s->nops, line = s->line, id = s->mnem->id;

    for (i = 0; i < n; i++)
        if (!parse_operand(A, s->op_beg[i], s->op_end[i], line, &o[i])) return 0;

    if (s->rep) {
        if (!s->mnem->is_string) { add_error(A, line, 0, "REP can only be used with string instructions (MOVSB, STOSB...)"); return 0; }
        eb(e, s->rep);
    }

#define NEED(k) do { if (n != (k)) { add_error(A, line, 0, "%s takes %d operand%s", s->mnem->name, (k), (k) == 1 ? "" : "s"); return 0; } } while (0)
#define BAD() do { add_error(A, line, 0, "invalid combination of operands for %s", s->mnem->name); return 0; } while (0)

    for (i = 0; i < n; i++) {
        if ((o[i].jmod == J_FAR || o[i].far_imm) && id != M_JMP && id != M_CALL) {
            add_error(A, line, 0, "FAR and segment:offset can only be used on JMP and CALL");
            return 0;
        }
        if (o[i].type == O_IMM && o[i].jmod && id != M_JMP && id != M_JCC && id != M_CALL &&
            id != M_LOOP && id != M_LOOPE && id != M_LOOPNE && id != M_JCXZ) {
            add_error(A, line, 0, "SHORT/NEAR can only be used on jumps");
            return 0;
        }
    }

    switch (id) {
    case M_SIMPLE:
        NEED(0);
        eb(e, s->mnem->cc_or_byte);
        return 1;

    case M_ADD: case M_OR: case M_ADC: case M_SBB: case M_AND: case M_SUB: case M_XOR: case M_CMP: {
        int aop = s->mnem->cc_or_byte, sz;
        NEED(2);
        if (!is_rm(&o[0])) BAD();
        if (o[0].type == O_MEM && o[1].type == O_MEM) { add_error(A, line, 0, "two memory operands are not allowed"); return 0; }
        if (o[1].type == O_SREG) BAD();
        sz = op_size2(A, line, &o[0], &o[1]);
        if (!sz) return 0;
        if (o[1].type == O_IMM) {
            const opnd_t *d = &o[0];
            val_t v = o[1].imm;
            if (sz == 1) {
                if (!check_byte(A, line, v)) return 0;
                if (o[1].imm_size == 2) BAD();
                if (d->type == O_REG8 && d->reg == 0) { eb(e, (aop << 3) | 4); eb(e, (int)v.v); return 1; }
                seg_prefix(e, d); eb(e, 0x80); modrm(A, s, e, aop, d, 0); eb(e, (int)v.v);
                return 1;
            }
            if (!check_word(A, line, v)) return 0;
            if (o[1].imm_size == 1) {
                if (v.reloc || (!v.unknown && !fits_simm8_word(v))) { add_error(A, line, 0, "value does not fit in a signed byte"); return 0; }
            }
            if (!(s->sticky & 8) && fits_simm8_word(v)) {
                seg_prefix(e, d); eb(e, 0x83); modrm(A, s, e, aop, d, 0); eb(e, (int)(v.v & 0xFF));
                return 1;
            }
            if (A->pass >= STICKY_PASS) s->sticky |= 8;
            if (d->type == O_REG16 && d->reg == 0) { eb(e, (aop << 3) | 5); ew(e, v.v); return 1; }
            seg_prefix(e, d); eb(e, 0x81); modrm(A, s, e, aop, d, 0); ew(e, v.v);
            return 1;
        }
        if (is_reg(&o[1])) {
            seg_prefix(e, &o[0]); eb(e, (aop << 3) | (sz == 2)); modrm(A, s, e, o[1].reg, &o[0], 0);
            return 1;
        }
        if (o[1].type == O_MEM && is_reg(&o[0])) {
            seg_prefix(e, &o[1]); eb(e, (aop << 3) | 2 | (sz == 2)); modrm(A, s, e, o[0].reg, &o[1], 1);
            return 1;
        }
        BAD();
    }

    case M_MOV: {
        int sz;
        NEED(2);
        if (o[0].type == O_MEM && o[1].type == O_MEM) { add_error(A, line, 0, "two memory operands are not allowed"); return 0; }
        if (o[0].type == O_SREG) {
            if (o[0].reg == 1) { add_error(A, line, 0, "CS cannot be written with MOV; on the 8086 it changes with a far jump"); return 0; }
            if (o[1].type == O_IMM) { add_error(A, line, 0, "the 8086 cannot load an immediate into a segment register; go through a register (mov ax, value / mov %s, ax)", REGS[16 + o[0].reg].name); return 0; }
            if (o[1].type == O_SREG) { add_error(A, line, 0, "cannot copy between segment registers; use an intermediate register"); return 0; }
            if (o[1].type == O_REG8 || (o[1].type == O_MEM && o[1].size == 1)) BAD();
            seg_prefix(e, &o[1]); eb(e, 0x8E); modrm(A, s, e, o[0].reg, &o[1], 1);
            return 1;
        }
        if (o[1].type == O_SREG) {
            if (o[0].type == O_REG8 || (o[0].type == O_MEM && o[0].size == 1) || o[0].type == O_IMM) BAD();
            seg_prefix(e, &o[0]); eb(e, 0x8C); modrm(A, s, e, o[1].reg, &o[0], 0);
            return 1;
        }
        if (!is_rm(&o[0])) BAD();
        sz = op_size2(A, line, &o[0], &o[1]);
        if (!sz) return 0;
        if (o[1].type == O_IMM) {
            val_t v = o[1].imm;
            if (sz == 1) { if (!check_byte(A, line, v)) return 0; }
            else if (!check_word(A, line, v)) return 0;
            if (is_reg(&o[0])) {
                eb(e, (sz == 1 ? 0xB0 : 0xB8) + o[0].reg);
                if (sz == 1) eb(e, (int)v.v); else ew(e, v.v);
                return 1;
            }
            seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0xC6 : 0xC7); modrm(A, s, e, 0, &o[0], 0);
            if (sz == 1) eb(e, (int)v.v); else ew(e, v.v);
            return 1;
        }
        /* short form AL/AX <-> [direct address] */
        if (is_reg(&o[0]) && o[0].reg == 0 && o[1].type == O_MEM && o[1].base < 0 && o[1].index < 0) {
            seg_prefix(e, &o[1]); eb(e, sz == 1 ? 0xA0 : 0xA1); ew(e, o[1].disp.v);
            return 1;
        }
        if (is_reg(&o[1]) && o[1].reg == 0 && o[0].type == O_MEM && o[0].base < 0 && o[0].index < 0) {
            seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0xA2 : 0xA3); ew(e, o[0].disp.v);
            return 1;
        }
        if (is_reg(&o[1])) {
            seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0x88 : 0x89); modrm(A, s, e, o[1].reg, &o[0], 0);
            return 1;
        }
        if (o[1].type == O_MEM && is_reg(&o[0])) {
            seg_prefix(e, &o[1]); eb(e, sz == 1 ? 0x8A : 0x8B); modrm(A, s, e, o[0].reg, &o[1], 1);
            return 1;
        }
        BAD();
    }

    case M_XCHG: {
        int sz;
        NEED(2);
        if (!is_rm(&o[0]) || !is_rm(&o[1])) BAD();
        if (o[0].type == O_MEM && o[1].type == O_MEM) { add_error(A, line, 0, "two memory operands are not allowed"); return 0; }
        sz = op_size2(A, line, &o[0], &o[1]);
        if (!sz) return 0;
        if (sz == 2 && o[0].type == O_REG16 && o[1].type == O_REG16 && (o[0].reg == 0 || o[1].reg == 0)) {
            eb(e, 0x90 + (o[0].reg == 0 ? o[1].reg : o[0].reg));
            return 1;
        }
        if (is_reg(&o[0])) {
            seg_prefix(e, &o[1]); eb(e, sz == 1 ? 0x86 : 0x87); modrm(A, s, e, o[0].reg, &o[1], 1);
        } else {
            seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0x86 : 0x87); modrm(A, s, e, o[1].reg, &o[0], 0);
        }
        return 1;
    }

    case M_TEST: {
        int sz;
        NEED(2);
        if (!is_rm(&o[0])) BAD();
        if (o[0].type == O_MEM && o[1].type == O_MEM) { add_error(A, line, 0, "two memory operands are not allowed"); return 0; }
        sz = op_size2(A, line, &o[0], &o[1]);
        if (!sz) return 0;
        if (o[1].type == O_IMM) {
            val_t v = o[1].imm;
            if (sz == 1) { if (!check_byte(A, line, v)) return 0; }
            else if (!check_word(A, line, v)) return 0;
            if (is_reg(&o[0]) && o[0].reg == 0) {
                eb(e, sz == 1 ? 0xA8 : 0xA9);
            } else {
                seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0xF6 : 0xF7); modrm(A, s, e, 0, &o[0], 0);
            }
            if (sz == 1) eb(e, (int)v.v); else ew(e, v.v);
            return 1;
        }
        if (!is_rm(&o[1])) BAD();
        if (o[1].type == O_MEM) {
            seg_prefix(e, &o[1]); eb(e, sz == 1 ? 0x84 : 0x85); modrm(A, s, e, o[0].reg, &o[1], 1);
        } else {
            seg_prefix(e, &o[0]); eb(e, sz == 1 ? 0x84 : 0x85); modrm(A, s, e, o[1].reg, &o[0], 0);
        }
        return 1;
    }

    case M_LDS:
        NEED(2);
        if (o[0].type != O_REG16 || o[1].type != O_MEM) {
            add_error(A, line, 0, "%s takes a 16-bit register and a far pointer in memory: %s si, [label]",
                      s->mnem->name, s->mnem->name);
            return 0;
        }
        if (o[1].size) {
            add_error(A, line, 0, "%s takes the address without a size: %s si, [label]", s->mnem->name, s->mnem->name);
            return 0;
        }
        seg_prefix(e, &o[1]); eb(e, s->mnem->cc_or_byte); modrm(A, s, e, o[0].reg, &o[1], 1);
        return 1;

    case M_LEA:
        NEED(2);
        if (o[0].type != O_REG16 || o[1].type != O_MEM) {
            add_error(A, line, 0, "LEA takes a 16-bit register and an address: lea si, [label]");
            return 0;
        }
        seg_prefix(e, &o[1]); eb(e, 0x8D); modrm(A, s, e, o[0].reg, &o[1], 1);
        return 1;

    case M_INC: case M_DEC: {
        int sub = s->mnem->cc_or_byte;
        NEED(1);
        if (o[0].type == O_REG16) { eb(e, (sub ? 0x48 : 0x40) + o[0].reg); return 1; }
        if (!is_rm(&o[0])) BAD();
        if (!o[0].size) { add_error(A, line, 0, "operation size not specified: use 'byte [...]' or 'word [...]'"); return 0; }
        seg_prefix(e, &o[0]); eb(e, o[0].size == 1 ? 0xFE : 0xFF); modrm(A, s, e, sub, &o[0], 0);
        return 1;
    }

    case M_NOT: case M_NEG: case M_MUL: case M_IMUL: case M_DIV: case M_IDIV:
        if (id == M_IMUL && n > 1) { add_error(A, line, 0, "IMUL with more than one operand is 80186+; on the 8086 use 'imul source' (result in AX or DX:AX)"); return 0; }
        NEED(1);
        if (!is_rm(&o[0])) BAD();
        if (!o[0].size) { add_error(A, line, 0, "operation size not specified: use 'byte [...]' or 'word [...]'"); return 0; }
        seg_prefix(e, &o[0]); eb(e, o[0].size == 1 ? 0xF6 : 0xF7); modrm(A, s, e, s->mnem->cc_or_byte, &o[0], 0);
        return 1;

    case M_ROL: case M_ROR: case M_RCL: case M_RCR: case M_SHL: case M_SHR: case M_SAR: {
        int op;
        NEED(2);
        if (!is_rm(&o[0])) BAD();
        if (!o[0].size) { add_error(A, line, 0, "operation size not specified: use 'byte [...]' or 'word [...]'"); return 0; }
        if (o[1].type == O_REG8 && o[1].reg == 1) op = 0xD2;
        else if (o[1].type == O_IMM && !o[1].imm.unknown && o[1].imm.v == 1 && !o[1].imm.reloc) op = 0xD0;
        else if (o[1].type == O_IMM && o[1].imm.unknown && !A->final) op = 0xD0;
        else { add_error(A, line, 0, "the 8086 can only shift by 1 or by CL (e.g. mov cl, 4 / shl ax, cl)"); return 0; }
        seg_prefix(e, &o[0]); eb(e, op | (o[0].size == 2)); modrm(A, s, e, s->mnem->cc_or_byte, &o[0], 0);
        return 1;
    }

    case M_PUSH: case M_POP: {
        int pop = id == M_POP;
        NEED(1);
        if (o[0].type == O_REG16) { eb(e, (pop ? 0x58 : 0x50) + o[0].reg); return 1; }
        if (o[0].type == O_SREG) {
            if (pop && o[0].reg == 1) { add_error(A, line, 0, "POP CS does not exist; CS changes with a far jump"); return 0; }
            eb(e, (o[0].reg << 3) | (pop ? 7 : 6));
            return 1;
        }
        if (o[0].type == O_REG8 || (o[0].type == O_MEM && o[0].size == 1)) { add_error(A, line, 0, "the 8086 stack only holds 16-bit words"); return 0; }
        if (o[0].type == O_IMM) { add_error(A, line, 0, "PUSH with an immediate is 80186+; on the 8086: mov ax, value / push ax"); return 0; }
        if (!o[0].size) { add_error(A, line, 0, "operation size not specified: %s word [...]", s->mnem->name); return 0; }
        seg_prefix(e, &o[0]);
        if (pop) { eb(e, 0x8F); modrm(A, s, e, 0, &o[0], 0); }
        else { eb(e, 0xFF); modrm(A, s, e, 6, &o[0], 0); }
        return 1;
    }

    case M_JMP: case M_CALL:
        NEED(1);
        if (o[0].far_imm) {
            if (o[0].jmod == J_SHORT || o[0].jmod == J_NEAR) BAD();
            if (o[0].jmod == J_FAR) {
                add_error(A, line, 0, "segment:offset is already far; write it without FAR: %s 0000h:7C00h", s->mnem->name);
                return 0;
            }
            if (!check_word(A, line, o[0].far_seg) || !check_word(A, line, o[0].imm)) return 0;
            eb(e, id == M_CALL ? 0x9A : 0xEA); ew(e, o[0].imm.v); ew(e, o[0].far_seg.v);
            return 1;
        }
        if (o[0].jmod == J_FAR) {
            if (o[0].type == O_IMM) {
                add_error(A, line, 0, "a far %s needs segment:offset (%s 0000h:7C00h) or a pointer in memory (%s far [bx])",
                          s->mnem->name, s->mnem->name, s->mnem->name);
                return 0;
            }
            if (o[0].type != O_MEM || o[0].size == 1) BAD();
            seg_prefix(e, &o[0]); eb(e, 0xFF); modrm(A, s, e, id == M_CALL ? 3 : 5, &o[0], 0);
            return 1;
        }
        if (o[0].type == O_IMM) {
            if (id == M_CALL) {
                if (o[0].jmod == J_SHORT) { add_error(A, line, 0, "CALL has no SHORT form"); return 0; }
                eb(e, 0xE8); ew(e, o[0].imm.v - (A->addr + e->n + 2));
                return 1;
            }
            {
                int64_t rel = o[0].imm.v - (A->addr + e->n + 2);
                int use_short;
                if (o[0].jmod == J_SHORT) use_short = 1;
                else if (o[0].jmod == J_NEAR) use_short = 0;
                else use_short = !(s->sticky & 1) && (o[0].imm.unknown || fits_s8(rel));
                if (use_short) { encode_rel8(A, s, e, 0xEB, &o[0], "short jump", " (remove SHORT to allow a near jump)"); return 1; }
                if (o[0].jmod == J_DEFAULT && A->pass >= STICKY_PASS) s->sticky |= 1;
                eb(e, 0xE9); ew(e, o[0].imm.v - (A->addr + e->n + 2));
                return 1;
            }
        }
        if (o[0].type == O_REG8 || o[0].type == O_SREG || (o[0].type == O_MEM && o[0].size == 1)) BAD();
        if (o[0].jmod == J_SHORT) BAD();
        seg_prefix(e, &o[0]); eb(e, 0xFF); modrm(A, s, e, id == M_CALL ? 2 : 4, &o[0], 0);
        return 1;

    case M_JCC:
        NEED(1);
        if (o[0].type != O_IMM) { add_error(A, line, 0, "conditional jumps only accept a label as target"); return 0; }
        if (o[0].jmod == J_NEAR) {
            /* The 8086 has only short conditional jumps (and NASM refuses
             * NEAR ones for it): NEAR is the short jump when the target is
             * close, else the opposite condition over a near JMP. */
            int64_t rel = o[0].imm.v - (A->addr + e->n + 2);
            if (!(s->sticky & 1) && (o[0].imm.unknown || fits_s8(rel))) {
                encode_rel8(A, s, e, 0x70 + s->mnem->cc_or_byte, &o[0], "conditional jump", "");
                return 1;
            }
            if (A->pass >= STICKY_PASS) s->sticky |= 1;
            eb(e, 0x70 + (s->mnem->cc_or_byte ^ 1)); eb(e, 3); eb(e, 0xE9);
            ew(e, o[0].imm.v - (A->addr + e->n + 2));
            return 1;
        }
        encode_rel8(A, s, e, 0x70 + s->mnem->cc_or_byte, &o[0], "conditional jump",
                    ". To go further, write NEAR (je near label)");
        return 1;

    case M_LOOP: case M_LOOPE: case M_LOOPNE: case M_JCXZ:
        NEED(1);
        if (o[0].type != O_IMM) BAD();
        if (o[0].jmod == J_NEAR) BAD();
        encode_rel8(A, s, e, id == M_LOOP ? 0xE2 : id == M_LOOPE ? 0xE1 : id == M_LOOPNE ? 0xE0 : 0xE3,
                    &o[0], "loop jump", ". For longer loops use dec cx / jnz with a JMP");
        return 1;

    case M_RET:
        if (n == 0) { eb(e, 0xC3); return 1; }
        NEED(1);
        if (o[0].type != O_IMM || !check_word(A, line, o[0].imm)) BAD();
        eb(e, 0xC2); ew(e, o[0].imm.v);
        return 1;

    case M_RETF:
        if (n == 0) { eb(e, 0xCB); return 1; }
        NEED(1);
        if (o[0].type != O_IMM || !check_word(A, line, o[0].imm)) BAD();
        eb(e, 0xCA); ew(e, o[0].imm.v);
        return 1;

    case M_IRET:
        NEED(0); eb(e, 0xCF); return 1;

    case M_INT3:
        NEED(0); eb(e, 0xCC); return 1;

    case M_INT:
        NEED(1);
        if (o[0].type != O_IMM) BAD();
        if (!o[0].imm.unknown && (o[0].imm.v < 0 || o[0].imm.v > 255)) { add_error(A, line, 0, "interrupt number must be 0..255"); return 0; }
        eb(e, 0xCD); eb(e, (int)o[0].imm.v);
        return 1;

    case M_IN: case M_OUT: {
        const opnd_t *acc = id == M_IN ? &o[0] : &o[1];
        const opnd_t *port = id == M_IN ? &o[1] : &o[0];
        int w;
        NEED(2);
        if (!is_reg(acc) || acc->reg != 0) { add_error(A, line, 0, "%s only works with AL or AX", s->mnem->name); return 0; }
        w = acc->type == O_REG16;
        if (port->type == O_REG16 && port->reg == 2) { eb(e, (id == M_IN ? 0xEC : 0xEE) | w); return 1; }
        if (port->type == O_IMM) {
            if (!port->imm.unknown && (port->imm.v < 0 || port->imm.v > 255)) {
                add_error(A, line, 0, "direct port numbers are 0..255; for higher ports use DX (mov dx, port / %s ...)", s->mnem->name);
                return 0;
            }
            eb(e, (id == M_IN ? 0xE4 : 0xE6) | w); eb(e, (int)port->imm.v);
            return 1;
        }
        add_error(A, line, 0, "port must be a number (0..255) or DX");
        return 0;
    }
    }
    BAD();
#undef NEED
#undef BAD
}

/* ====================================================================== */
/* Statement parsing                                                       */
/* ====================================================================== */

/* Scans an expression without evaluating it, to find where it ends. */
static int skip_expr(asm_t *A, int pos, int end, int line)
{
    ex_t x;
    int saved = A->final;
    asm86_result_t *r = A->res;
    int ec = r->error_count;
    x.A = A; x.pos = pos; x.end = end; x.line = line; x.err = 0;
    A->final = 0;
    ex_or(&x);
    A->final = saved;
    r->error_count = ec;
    return x.err ? -1 : x.pos;
}

static void split_operands(asm_t *A, stmt_t *s, int beg, int end)
{
    int depth = 0, i, start = beg;
    s->nops = 0;
    if (beg >= end) return;
    for (i = beg; i <= end; i++) {
        const tok_t *t = &A->toks[i];
        int at_end = (i == end);
        if (!at_end && t->type == TK_PUNCT && (t->punct == '[' || t->punct == '(')) depth++;
        if (!at_end && t->type == TK_PUNCT && (t->punct == ']' || t->punct == ')')) depth--;
        if (at_end || (depth == 0 && t->type == TK_PUNCT && t->punct == ',')) {
            if (s->nops >= MAX_OPS) {
                add_error(A, s->line, 1, "too many operands");
                s->kind = K_ERROR;
                return;
            }
            if (start == i) {
                add_error(A, s->line, 1, "missing operand");
                s->kind = K_ERROR;
                return;
            }
            s->op_beg[s->nops] = start;
            s->op_end[s->nops] = i;
            s->nops++;
            start = i + 1;
        }
    }
}

static void parse_stmt(asm_t *A, stmt_t *s, int i, int end)
{
    const tok_t *t;
    int d;

    if (i >= end) { s->kind = K_EMPTY; return; }
    t = &A->toks[i];
    if (t->type != TK_ID) {
        add_error(A, s->line, 1, "expected an instruction or a label");
        s->kind = K_ERROR;
        return;
    }

    d = find_directive(t);
    if (d == D_TIMES) {
        int e2;
        if (s->times_beg >= 0) { add_error(A, s->line, 1, "repeated TIMES"); s->kind = K_ERROR; return; }
        e2 = skip_expr(A, i + 1, end, s->line);
        if (e2 < 0 || e2 == i + 1) { add_error(A, s->line, 1, "TIMES needs a count"); s->kind = K_ERROR; return; }
        s->times_beg = i + 1;
        s->times_end = e2;
        parse_stmt(A, s, e2, end);
        if (s->kind == K_EQU || s->kind == K_ORG || s->kind == K_IGNORE || s->kind == K_SECTION) {
            add_error(A, s->line, 1, "TIMES cannot be used with this directive");
            s->kind = K_ERROR;
        }
        return;
    }
    if (d == D_REP || d == D_REPNE) {
        s->rep = (uint8_t)(d == D_REP ? 0xF3 : 0xF2);
        i++;
        if (i >= end || !find_mnem(&A->toks[i])) {
            add_error(A, s->line, 1, "REP must be followed by a string instruction");
            s->kind = K_ERROR;
            return;
        }
        t = &A->toks[i];
        d = D_NONE;
    }
    s->args_beg = i + 1;
    s->args_end = end;

    switch (d) {
    case D_DB: s->kind = K_DB; return;
    case D_DW: s->kind = K_DW; return;
    case D_RESB: s->kind = K_RESB; return;
    case D_RESW: s->kind = K_RESW; return;
    case D_EQU:
        if (s->label < 0) { add_error(A, s->line, 1, "EQU needs a name in front: WIDTH equ 320"); s->kind = K_ERROR; return; }
        s->kind = K_EQU;
        return;
    case D_ORG: s->kind = K_ORG; return;
    case D_BITS:
        if (end - i != 2 || A->toks[i + 1].type != TK_NUM || A->toks[i + 1].num != 16) {
            add_error(A, s->line, 1, "this machine only has 16-bit mode (bits 16)");
            s->kind = K_ERROR;
            return;
        }
        s->kind = K_IGNORE;
        return;
    case D_CPU:
        if (end - i != 2 || !(A->toks[i + 1].type == TK_NUM && A->toks[i + 1].num == 8086)) {
            add_error(A, s->line, 1, "this machine is an 8086 (cpu 8086)");
            s->kind = K_ERROR;
            return;
        }
        s->kind = K_IGNORE;
        return;
    case D_OTHER_DATA:
        add_error(A, s->line, 1, "'%.*s' is not supported; use db/dw (bytes and words)", t->len, t->s);
        s->kind = K_ERROR;
        return;
    case D_SECTION: {
        int k;
        const tok_t *n = &A->toks[i + 1];
        if (end - i < 2 || n->type != TK_ID) {
            add_error(A, s->line, 1, "SECTION needs a name: .text, .data or .bss");
            s->kind = K_ERROR;
            return;
        }
        for (k = 0; k < SEC_COUNT; k++)
            if ((int)strlen(SECTION_NAMES[k]) == n->len && !memcmp(SECTION_NAMES[k], n->s, (size_t)n->len)) break;
        if (k == SEC_COUNT) {
            add_error(A, s->line, 1, "only the .text, .data and .bss sections are supported, not '%.*s'", n->len, n->s);
            s->kind = K_ERROR;
            return;
        }
        if (end - i > 2) {
            add_error(A, s->line, 1, "section attributes (align=, start=, vstart=...) are not supported");
            s->kind = K_ERROR;
            return;
        }
        s->kind = K_SECTION;
        s->section = (uint8_t)k;
        return;
    }
    }

    if (in_list(t, UNSUPPORTED_8086)) {
        add_error(A, s->line, 1, "instruction not supported on this machine: %.*s", t->len, t->s);
        s->kind = K_ERROR;
        return;
    }
    if (in_list(t, LATER_CPUS)) {
        add_error(A, s->line, 1, "%.*s does not exist on the 8086 (it is from a later CPU)", t->len, t->s);
        s->kind = K_ERROR;
        return;
    }
    s->mnem = find_mnem(t);
    if (!s->mnem) {
        add_error(A, s->line, 1, "unknown instruction: %.*s", t->len, t->s);
        s->kind = K_ERROR;
        return;
    }
    s->kind = K_INSTR;
    split_operands(A, s, i + 1, end);
}

static void parse_line(asm_t *A, int beg, int end, int line)
{
    stmt_t *s;
    int i = beg;
    A->st = (stmt_t *)grow(A->st, &A->capst, A->nst + 1, sizeof(stmt_t));
    s = &A->st[A->nst++];
    memset(s, 0, sizeof(*s));
    s->line = line;
    s->label = -1;
    s->times_beg = s->times_end = -1;

    if (i < end && A->toks[i].type == TK_ID && !is_keyword(&A->toks[i]) && !find_reg(&A->toks[i])) {
        if (i + 1 < end && A->toks[i + 1].type == TK_PUNCT && A->toks[i + 1].punct == ':') {
            s->label = i; i += 2;
        } else if (i + 1 == end || (A->toks[i + 1].type == TK_ID && is_keyword(&A->toks[i + 1]))) {
            s->label = i; i += 1;
        } else {
            add_error(A, line, 1, "unknown instruction: %.*s", A->toks[i].len, A->toks[i].s);
            s->kind = K_ERROR;
            return;
        }
    }
    parse_stmt(A, s, i, end);
}

/* ====================================================================== */
/* Passes                                                                  */
/* ====================================================================== */

static void out_bytes(asm_t *A, const uint8_t *b, size_t n)
{
    size_t k;
    if (!A->final || A->sec == SEC_BSS) { A->wpos += (int64_t)n; return; }
    for (k = 0; k < n; k++, A->wpos++) {
        int64_t at = A->wpos - A->org;
        if (at < 0 || at > 0xFFFF) continue;   /* reported: does not fit in 64 KB */
        A->out[at] = b[k];
        if ((size_t)at + 1 > A->outlen) A->outlen = (size_t)at + 1;
    }
}

static void out_zero(asm_t *A, size_t n)
{
    static const uint8_t z[64] = {0};
    while (n) { size_t k = n > 64 ? 64 : n; out_bytes(A, z, k); n -= k; }
}

/* db / dw: comma-separated list of expressions and strings */
static int64_t emit_data(asm_t *A, stmt_t *s, int word)
{
    int i = s->args_beg, end = s->args_end;
    int64_t size = 0;
    if (i >= end) { add_error(A, s->line, 0, "%s needs at least one value", word ? "dw" : "db"); return 0; }
    while (i < end) {
        int j = i, depth = 0;
        while (j < end) {
            const tok_t *t = &A->toks[j];
            if (t->type == TK_PUNCT && t->punct == '(') depth++;
            if (t->type == TK_PUNCT && t->punct == ')') depth--;
            if (depth == 0 && t->type == TK_PUNCT && t->punct == ',') break;
            j++;
        }
        if (j == i) { add_error(A, s->line, 0, "missing value between commas"); return size; }
        if (j - i == 1 && A->toks[i].type == TK_STR && (A->toks[i].len != 1 || word) &&
            !(word && A->toks[i].len == 2)) {
            const tok_t *t = &A->toks[i];
            out_bytes(A, (const uint8_t *)t->s, (size_t)t->len);
            size += t->len;
            if (word && (t->len & 1)) { out_zero(A, 1); size++; }
        } else {
            int err;
            val_t v = eval_range(A, i, j, s->line, &err);
            uint8_t b[2];
            if (!err) {
                if (word) check_word(A, s->line, v);
                else check_byte(A, s->line, v);
            }
            b[0] = (uint8_t)(v.v & 0xFF);
            b[1] = (uint8_t)((v.v >> 8) & 0xFF);
            out_bytes(A, b, word ? 2 : 1);
            size += word ? 2 : 1;
        }
        i = j + 1;
        if (j < end && i == end) { add_error(A, s->line, 0, "trailing comma"); }
    }
    return size;
}

static int64_t eval_count(asm_t *A, stmt_t *s, int beg, int end, const char *what)
{
    int err;
    val_t v = eval_range(A, beg, end, s->line, &err);
    if (err || v.unknown) return 0;
    if (v.reloc) { add_error(A, s->line, 0, "%s needs a number, not an address (try $-$$)", what); return 0; }
    if (v.v < 0) { add_error(A, s->line, 0, "%s cannot be negative (%lld)", what, (long long)v.v); return 0; }
    if (v.v > 0x10000) { add_error(A, s->line, 0, "%s too large (%lld)", what, (long long)v.v); return 0; }
    return v.v;
}

static void record_line(asm_t *A, stmt_t *s, int64_t start, int64_t size)
{
    if (!A->final || size <= 0) return;
    A->lines = (asm86_line_t *)grow(A->lines, &A->caplines, A->nlines + 1, sizeof(asm86_line_t));
    A->lines[A->nlines].line = s->line;
    A->lines[A->nlines].address = (uint16_t)start;
    A->lines[A->nlines].size = (uint16_t)(size > 0xFFFF ? 0xFFFF : size);
    A->nlines++;
}

static void run_pass(asm_t *A)
{
    int k;
    /* the sections, with the sizes of the last pass */
    A->sec_base[SEC_TEXT] = A->org;
    A->sec_base[SEC_DATA] = (A->sec_base[SEC_TEXT] + A->sec_size[SEC_TEXT] + 3) & ~(int64_t)3;
    A->sec_base[SEC_BSS] = (A->sec_base[SEC_DATA] + A->sec_size[SEC_DATA] + 3) & ~(int64_t)3;
    for (k = 0; k < SEC_COUNT; k++) A->sec_addr[k] = A->sec_base[k];
    A->sec = SEC_TEXT;
    A->addr = A->org;
    A->last_global = -1;
    A->changed = 0;

    for (k = 0; k < A->nst; k++) {
        stmt_t *s = &A->st[k];
        int64_t start = A->addr, reps = 1, r;
        A->cur_line = s->line;
        A->stmt_failed = 0;
        A->line_start = A->addr;

        if (s->label >= 0 && s->kind != K_EQU) {
            const tok_t *lt = &A->toks[s->label];
            val_t v;
            v.v = A->addr; v.reloc = 1; v.unknown = 0;
            define_sym(A, lt, v, 1, s->line);
            if (!(lt->s[0] == '.')) A->last_global = s->label;
        }

        switch (s->kind) {
        case K_EMPTY: case K_IGNORE: case K_ERROR:
            break;  /* lines with syntax errors take 0 bytes */
        case K_ORG: {
            int err;
            val_t v = eval_range(A, s->args_beg, s->args_end, s->line, &err);
            if (err) break;
            if (v.reloc || v.unknown) { add_error(A, s->line, 0, "ORG needs a number"); break; }
            if (k != 0 && A->addr != A->org) {
                int j, code_before = 0;
                for (j = 0; j < k; j++) if (A->st[j].size) code_before = 1;
                if (code_before) { add_error(A, s->line, 0, "ORG must come before any code"); break; }
            }
            if (A->org_set && A->org_set != s->line) { add_error(A, s->line, 0, "only one ORG is allowed"); break; }
            if (v.v < 0 || v.v > 0xFFFF) { add_error(A, s->line, 0, "ORG out of range"); break; }
            if (A->sec != SEC_TEXT) { add_error(A, s->line, 0, "ORG goes in the .text section"); break; }
            A->org_set = s->line;
            if (A->org != v.v) { A->org = v.v; A->changed = 1; }
            A->addr = A->org;
            A->sec_base[SEC_TEXT] = A->org;
            break;
        }
        case K_SECTION:
            A->sec_addr[A->sec] = A->addr;
            A->sec = s->section;
            A->addr = A->sec_addr[A->sec];
            break;
        case K_EQU: {
            int err;
            val_t v = eval_range(A, s->args_beg, s->args_end, s->line, &err);
            const tok_t *lt = &A->toks[s->label];
            if (err) v.unknown = 1;
            define_sym(A, lt, v, 0, s->line);
            if (!(lt->s[0] == '.')) A->last_global = s->label;
            break;
        }
        default:
            if (A->sec == SEC_BSS && s->kind != K_RESB && s->kind != K_RESW) {
                add_error(A, s->line, 0, "only RESB and RESW go in .bss (it takes no room in the file)");
                break;
            }
            if (s->times_beg >= 0) reps = eval_count(A, s, s->times_beg, s->times_end, "TIMES");
            for (r = 0; r < reps; r++) {
                int64_t sz = 0;
                A->wpos = A->addr;
                if (s->kind == K_INSTR) {
                    enc_t e;
                    e.n = 0;
                    if (encode_instr(A, s, &e)) { out_bytes(A, e.b, (size_t)e.n); sz = e.n; }
                } else if (s->kind == K_DB || s->kind == K_DW) {
                    sz = emit_data(A, s, s->kind == K_DW);
                } else {
                    int64_t cnt = eval_count(A, s, s->args_beg, s->args_end, s->kind == K_RESB ? "RESB" : "RESW");
                    sz = cnt * (s->kind == K_RESW ? 2 : 1);
                    out_zero(A, (size_t)sz);
                }
                A->addr += sz;
                if (A->stmt_failed && !A->final) break;
            }
            break;
        }
        {
            int64_t sz = A->addr - start;
            if (s->kind != K_ORG && (uint16_t)sz != s->size) { s->size = (uint16_t)sz; A->changed = 1; }
            if (s->addr != (uint16_t)start) { s->addr = (uint16_t)start; A->changed = 1; }
            record_line(A, s, start, sz);
        }
        if (A->addr > 0x10000 && A->final) {
            add_error(A, s->line, 0, "the program does not fit in a 64 KB segment");
            return;
        }
    }
    A->sec_addr[A->sec] = A->addr;
    for (k = 0; k < SEC_COUNT; k++) {
        int64_t size = A->sec_addr[k] - A->sec_base[k];
        if (size != A->sec_size[k]) { A->sec_size[k] = size; A->changed = 1; }
    }
}

int asm86_assemble(const char *source, asm86_result_t *res)
{
    asm_t A;
    const char *p = source, *ls;
    int line = 1, i;

    memset(res, 0, sizeof(*res));
    memset(&A, 0, sizeof(A));
    A.src = source;
    A.res = res;
    A.last_global = -1;

    /* 1. Tokenize and parse every line */
    while (*p) {
        int beg = A.ntok;
        ls = p;
        while (*p && *p != '\n') p++;
        {
            const char *e = p;
            if (e > ls && e[-1] == '\r') e--;
            if (lex_line(&A, ls, e, line)) parse_line(&A, beg, A.ntok - 1, line);
        }
        if (*p == '\n') p++;
        line++;
    }

    /* 2. Passes until addresses and sizes stop changing. They run even when
     *    there are syntax errors, so that all errors are reported at once. */
    {
        int stable = 0;
        for (A.pass = 0; A.pass < MAX_PASSES; A.pass++) {
            run_pass(&A);
            if (!A.changed && A.pass > 0) { stable = 1; break; }
        }
        A.final = 1;
        A.out = (uint8_t *)calloc(0x10000, 1);
        A.outlen = 0;
        A.nlines = 0;
        A.pass++;
        run_pass(&A);
        if (!stable && res->error_count == 0)
            add_error(&A, 0, 1, "could not resolve addresses (circular references)");
        if (A.changed && res->error_count == 0)
            add_error(&A, 0, 1, "addresses did not stabilize; check expressions that depend on their own size");
    }

    /* Sort errors by line (syntax errors are found first) */
    for (i = 1; i < res->error_count; i++) {
        asm86_error_t tmp = res->errors[i];
        int j = i - 1;
        while (j >= 0 && res->errors[j].line > tmp.line) { res->errors[j + 1] = res->errors[j]; j--; }
        res->errors[j + 1] = tmp;
    }

    /* 3. Result */
    if (res->error_count == 0) {
        res->code = A.out;
        res->size = A.outlen;
        A.out = NULL;
    }
    res->origin = (uint16_t)A.org;
    res->lines = A.lines;
    res->line_count = A.nlines;
    A.lines = NULL;

    if (A.nsym) {
        res->symbols = (asm86_symbol_t *)calloc((size_t)A.nsym, sizeof(asm86_symbol_t));
        for (i = 0; i < A.nsym; i++) {
            snprintf(res->symbols[i].name, sizeof(res->symbols[i].name), "%s", A.syms[i].name);
            res->symbols[i].value = (int32_t)A.syms[i].value;
            res->symbols[i].is_label = A.syms[i].is_label;
        }
        res->symbol_count = A.nsym;
    }

    for (i = 0; i < A.nsym; i++) free(A.syms[i].name);
    free(A.syms);
    free(A.toks);
    free(A.st);
    free(A.out);
    free(A.lines);
    return res->error_count ? -1 : 0;
}

void asm86_free(asm86_result_t *res)
{
    if (!res) return;
    free(res->code);
    free(res->symbols);
    free(res->lines);
    res->code = NULL;
    res->symbols = NULL;
    res->lines = NULL;
}
