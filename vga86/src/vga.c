/*
 * vga86 - monochrome VGA display device. See include/vga.h.
 */
#include "vga.h"
#include "font_cp437.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MEM_MASK      0xFFFFFu
#define TEXT_BASE     0xB8000u
#define TEXT_SIZE     0x8000u          /* 32 KB window at B800:0000 */
#define GFX_BASE      0xA0000u
#define BDA           0x400u           /* BIOS data area, 0040:0000 */

#define COLS_TEXT     80
#define ROWS          25
#define COLS_GFX      40               /* mode 13h BIOS text: 8x8 cells */

#define FRAME_NS      ((uint64_t)VGA_FRAME_US * 1000u)
#define LINE_NS       (FRAME_NS / VGA_LINES)
#define ACTIVE_NS     (LINE_NS * 640u / 800u)   /* 640 of 800 dot clocks */
#define VRETRACE_LINE 412u                      /* retrace: lines 412-413 */
#define VRETRACE_END  414u
#define CRTC_REGS     0x19

typedef struct vga_state {
    uint8_t mode;
    uint8_t dac[256][3];            /* 6-bit R, G, B */
    uint8_t dac_write, dac_read;    /* indices */
    uint8_t dac_wcomp, dac_rcomp;   /* component 0..2 */
    uint8_t dac_pending[3];
    uint8_t dac_reading;            /* last index port written was 3C7h */
    uint8_t ac_pal[16];             /* attribute controller palette */
    uint8_t overscan;
    uint8_t crtc[CRTC_REGS];
    uint8_t crtc_index;
    uint8_t blink_enabled;
    uint64_t time_ns;
} vga_state_t;

struct vga {
    vga_state_t st;
    uint8_t *mem;
};

/* ---------- memory helpers ---------- */

static uint8_t rd(const vga_t *v, uint32_t a) { return v->mem[a & MEM_MASK]; }
static void wr(vga_t *v, uint32_t a, uint8_t b) { v->mem[a & MEM_MASK] = b; }
static void wr16(vga_t *v, uint32_t a, uint16_t w) { wr(v, a, (uint8_t)w); wr(v, a + 1, (uint8_t)(w >> 8)); }
static uint32_t lin(uint16_t seg, uint16_t off) { return (((uint32_t)seg << 4) + off) & MEM_MASK; }

/* ---------- palettes ---------- */

static const uint8_t EGA16[16][3] = {
    {0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0}, {42, 0, 42}, {42, 21, 0}, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}
};
static const uint8_t DEFAULT_AC[16] = {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};
static const uint8_t GRAYS[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};

/* Mode 03h: the 64 EGA colors (rgbRGB bits) in DAC entries 0-63. */
static void load_ega64(vga_t *v)
{
    int i;
    memset(v->st.dac, 0, sizeof(v->st.dac));
    for (i = 0; i < 64; i++) {
        v->st.dac[i][0] = (uint8_t)(((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21);
        v->st.dac[i][1] = (uint8_t)(((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21);
        v->st.dac[i][2] = (uint8_t)((i & 1) * 42 + ((i >> 3) & 1) * 21);
    }
}

/* Mode 13h: the standard VGA default 256-color palette. */
static void load_default256(vga_t *v)
{
    static const uint8_t levels[3][3][5] = {
        {{0, 16, 31, 47, 63}, {31, 39, 47, 55, 63}, {45, 49, 54, 58, 63}},
        {{0, 7, 14, 21, 28}, {14, 17, 21, 24, 28}, {20, 22, 24, 26, 28}},
        {{0, 4, 8, 12, 16}, {8, 10, 12, 14, 16}, {11, 12, 13, 15, 16}},
    };
    /* 24 hues from blue through magenta, red, yellow, green, cyan; values
     * are indices into the 5 levels of a saturation row: {R, G, B} */
    static const uint8_t hues[24][3] = {
        {0, 0, 4}, {1, 0, 4}, {2, 0, 4}, {3, 0, 4}, {4, 0, 4}, {4, 0, 3}, {4, 0, 2}, {4, 0, 1},
        {4, 0, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {3, 4, 0}, {2, 4, 0}, {1, 4, 0},
        {0, 4, 0}, {0, 4, 1}, {0, 4, 2}, {0, 4, 3}, {0, 4, 4}, {0, 3, 4}, {0, 2, 4}, {0, 1, 4},
    };
    int i, g, s, h, c, n = 32;
    memset(v->st.dac, 0, sizeof(v->st.dac));
    for (i = 0; i < 16; i++) {
        memcpy(v->st.dac[i], EGA16[i], 3);
        v->st.dac[16 + i][0] = v->st.dac[16 + i][1] = v->st.dac[16 + i][2] = GRAYS[i];
    }
    for (g = 0; g < 3; g++)
        for (s = 0; s < 3; s++)
            for (h = 0; h < 24; h++, n++)
                for (c = 0; c < 3; c++)
                    v->st.dac[n][c] = levels[g][s][hues[h][c]];
}

static uint8_t luma6(const uint8_t rgb[3])
{
    return (uint8_t)((30u * rgb[0] + 59u * rgb[1] + 11u * rgb[2] + 50u) / 100u);
}

static void build_luma(const vga_t *v, uint8_t out[256])
{
    int i;
    for (i = 0; i < 256; i++) out[i] = (uint8_t)((luma6(v->st.dac[i]) * 255u + 31u) / 63u);
}

/* ---------- BIOS data area ---------- */

static int text_mode(const vga_t *v) { return v->st.mode != 0x13; }
static int cols(const vga_t *v) { return text_mode(v) ? COLS_TEXT : COLS_GFX; }

static void get_cursor(const vga_t *v, int *col, int *row)
{
    *col = rd(v, BDA + 0x50);
    *row = rd(v, BDA + 0x51);
}

static void set_cursor(vga_t *v, int col, int row)
{
    uint16_t pos;
    wr(v, BDA + 0x50, (uint8_t)col);
    wr(v, BDA + 0x51, (uint8_t)row);
    pos = (uint16_t)(row * COLS_TEXT + col);
    v->st.crtc[0x0E] = (uint8_t)(pos >> 8);
    v->st.crtc[0x0F] = (uint8_t)pos;
}

static void set_mode(vga_t *v, uint8_t mode, int clear)
{
    vga_state_t *s = &v->st;
    uint32_t i;

    s->mode = mode;
    memset(s->crtc, 0, sizeof(s->crtc));
    memcpy(s->ac_pal, DEFAULT_AC, 16);
    s->overscan = 0;
    s->blink_enabled = 1;
    s->dac_write = s->dac_read = s->dac_wcomp = s->dac_rcomp = 0;
    s->dac_reading = 0;

    if (mode == 0x13) {
        load_default256(v);
        if (clear) for (i = 0; i < 0x10000; i++) wr(v, GFX_BASE + i, 0);
    } else {
        load_ega64(v);
        s->crtc[0x0A] = 0x0D;   /* cursor start scanline */
        s->crtc[0x0B] = 0x0E;   /* cursor end scanline */
        if (clear) for (i = 0; i < TEXT_SIZE; i += 2) wr16(v, TEXT_BASE + i, 0x0720);
    }

    wr(v, BDA + 0x49, mode);
    wr16(v, BDA + 0x4A, (uint16_t)cols(v));
    wr16(v, BDA + 0x4C, (uint16_t)(mode == 0x13 ? 0xFA00 : 0x1000));
    wr16(v, BDA + 0x4E, 0);
    for (i = 0; i < 16; i++) wr(v, BDA + 0x50 + i, 0);
    wr(v, BDA + 0x60, s->crtc[0x0B]);
    wr(v, BDA + 0x61, s->crtc[0x0A]);
    wr(v, BDA + 0x62, 0);
    wr16(v, BDA + 0x63, 0x3D4);
    wr(v, BDA + 0x84, ROWS - 1);
    wr16(v, BDA + 0x85, (uint16_t)(mode == 0x13 ? 8 : 16));
    set_cursor(v, 0, 0);
}

/* ---------- character output (page 0) ---------- */

static void put_cell(vga_t *v, int col, int row, uint8_t ch, int attr, uint8_t color)
{
    if (col < 0 || row < 0 || row >= ROWS || col >= cols(v)) return;
    if (text_mode(v)) {
        uint32_t a = TEXT_BASE + (uint32_t)(row * COLS_TEXT + col) * 2;
        wr(v, a, ch);
        if (attr >= 0) wr(v, a + 1, (uint8_t)attr);
    } else {
        int y, x;
        for (y = 0; y < 8; y++) {
            uint8_t bits = vga_font_8x8[ch][y];
            uint32_t a = GFX_BASE + (uint32_t)((row * 8 + y) * 320 + col * 8);
            for (x = 0; x < 8; x++) wr(v, a + (uint32_t)x, (bits & (0x80 >> x)) ? color : 0);
        }
    }
}

/* Scrolls the window [top..bottom] x [left..right] (in character cells).
 * up=1 moves content up. n=0 clears the window. fill = attribute or color. */
static void scroll(vga_t *v, int n, int up, int top, int left, int bottom, int right, uint8_t fill)
{
    int maxc = cols(v) - 1, h, r, c;
    if (right > maxc) right = maxc;
    if (bottom > ROWS - 1) bottom = ROWS - 1;
    if (top > bottom || left > right) return;
    h = bottom - top + 1;
    if (n == 0 || n > h) n = h;

    for (r = 0; r < h; r++) {
        int dst = up ? top + r : bottom - r;
        int src = up ? dst + n : dst - n;
        int from_src = r < h - n;
        if (text_mode(v)) {
            for (c = left; c <= right; c++) {
                uint32_t d = TEXT_BASE + (uint32_t)(dst * COLS_TEXT + c) * 2;
                if (from_src) {
                    uint32_t s = TEXT_BASE + (uint32_t)(src * COLS_TEXT + c) * 2;
                    wr(v, d, rd(v, s));
                    wr(v, d + 1, rd(v, s + 1));
                } else {
                    wr(v, d, ' ');
                    wr(v, d + 1, fill);
                }
            }
        } else {
            int y, x;
            for (y = 0; y < 8; y++) {
                uint32_t d = GFX_BASE + (uint32_t)((dst * 8 + y) * 320 + left * 8);
                uint32_t s = GFX_BASE + (uint32_t)((src * 8 + y) * 320 + left * 8);
                for (x = 0; x < (right - left + 1) * 8; x++)
                    wr(v, d + (uint32_t)x, from_src ? rd(v, s + (uint32_t)x) : fill);
            }
        }
    }
}

/* Teletype output, like INT 10h AH=0Eh. attr < 0 keeps the cell attribute. */
static void teletype(vga_t *v, uint8_t ch, int attr, uint8_t color)
{
    int col, row;
    get_cursor(v, &col, &row);
    switch (ch) {
    case 0x07: break;                         /* bell: no sound device here */
    case 0x08: if (col > 0) col--; break;
    case 0x0A: row++; break;
    case 0x0D: col = 0; break;
    default:
        put_cell(v, col, row, ch, attr, color);
        if (++col >= cols(v)) { col = 0; row++; }
        break;
    }
    if (row >= ROWS) {
        uint8_t fill = 0;
        if (text_mode(v)) {
            int c2, r2;
            get_cursor(v, &c2, &r2);
            fill = rd(v, TEXT_BASE + (uint32_t)(r2 * COLS_TEXT + c2) * 2 + 1);
        }
        scroll(v, 1, 1, 0, 0, ROWS - 1, cols(v) - 1, fill);
        row = ROWS - 1;
    }
    set_cursor(v, col, row);
}

/* ---------- INT 10h ---------- */

#define AH(r) ((uint8_t)((r)->ax >> 8))
#define AL(r) ((uint8_t)(r)->ax)
#define BH(r) ((uint8_t)((r)->bx >> 8))
#define BL(r) ((uint8_t)(r)->bx)
#define CH(r) ((uint8_t)((r)->cx >> 8))
#define CL(r) ((uint8_t)(r)->cx)
#define DH(r) ((uint8_t)((r)->dx >> 8))
#define DL(r) ((uint8_t)(r)->dx)
#define SET_HI(x, v) ((x) = (uint16_t)(((x) & 0x00FF) | ((uint16_t)(v) << 8)))
#define SET_LO(x, v) ((x) = (uint16_t)(((x) & 0xFF00) | (uint8_t)(v)))

static int int10_palette(vga_t *v, vga_bios_regs_t *r)
{
    vga_state_t *s = &v->st;
    uint32_t p;
    int i;
    switch (AL(r)) {
    case 0x00:
        if (BL(r) > 15) return 0;
        s->ac_pal[BL(r)] = (uint8_t)(BH(r) & 0x3F);
        return 1;
    case 0x01: s->overscan = BH(r); return 1;
    case 0x02:
        p = lin(r->es, r->dx);
        for (i = 0; i < 16; i++) s->ac_pal[i] = (uint8_t)(rd(v, p + (uint32_t)i) & 0x3F);
        s->overscan = rd(v, p + 16);
        return 1;
    case 0x03:
        if (BL(r) > 1) return 0;
        s->blink_enabled = BL(r);
        return 1;
    case 0x07:
        if (BL(r) == 0x11) SET_HI(r->bx, s->overscan);
        else if (BL(r) <= 15) SET_HI(r->bx, s->ac_pal[BL(r)]);
        else return 0;
        return 1;
    case 0x08: SET_HI(r->bx, s->overscan); return 1;
    case 0x09:
        p = lin(r->es, r->dx);
        for (i = 0; i < 16; i++) wr(v, p + (uint32_t)i, s->ac_pal[i]);
        wr(v, p + 16, s->overscan);
        return 1;
    case 0x10:
        s->dac[BL(r)][0] = (uint8_t)(DH(r) & 63);
        s->dac[BL(r)][1] = (uint8_t)(CH(r) & 63);
        s->dac[BL(r)][2] = (uint8_t)(CL(r) & 63);
        return 1;
    case 0x12:
        p = lin(r->es, r->dx);
        for (i = 0; i < r->cx; i++) {
            int idx = (r->bx + i) & 0xFF, c;
            for (c = 0; c < 3; c++) s->dac[idx][c] = (uint8_t)(rd(v, p + (uint32_t)(i * 3 + c)) & 63);
        }
        return 1;
    case 0x15:
        SET_HI(r->dx, s->dac[BL(r)][0]);
        SET_HI(r->cx, s->dac[BL(r)][1]);
        SET_LO(r->cx, s->dac[BL(r)][2]);
        return 1;
    case 0x17:
        p = lin(r->es, r->dx);
        for (i = 0; i < r->cx; i++) {
            int idx = (r->bx + i) & 0xFF, c;
            for (c = 0; c < 3; c++) wr(v, p + (uint32_t)(i * 3 + c), s->dac[idx][c]);
        }
        return 1;
    case 0x1B:   /* sum to gray scale, as the BIOS does for monochrome monitors */
        for (i = 0; i < r->cx; i++) {
            int idx = (r->bx + i) & 0xFF;
            uint8_t g = luma6(s->dac[idx]);
            s->dac[idx][0] = s->dac[idx][1] = s->dac[idx][2] = g;
        }
        return 1;
    default:
        return 0;
    }
}

int vga_int10(vga_t *v, vga_bios_regs_t *r)
{
    int col, row, i;
    switch (AH(r)) {
    case 0x00: {
        uint8_t m = AL(r) & 0x7F;
        if (m != 0x02 && m != 0x03 && m != 0x13) return 0;
        set_mode(v, m == 0x02 ? 0x03 : m, !(AL(r) & 0x80));
        return 1;
    }
    case 0x01:
        v->st.crtc[0x0A] = (uint8_t)(CH(r) & 0x3F);
        v->st.crtc[0x0B] = (uint8_t)(CL(r) & 0x1F);
        wr(v, BDA + 0x60, CL(r));
        wr(v, BDA + 0x61, CH(r));
        return 1;
    case 0x02:
        if (BH(r) != 0) return 0;
        set_cursor(v, DL(r), DH(r));
        return 1;
    case 0x03:
        if (BH(r) != 0) return 0;
        get_cursor(v, &col, &row);
        r->dx = (uint16_t)((row << 8) | col);
        r->cx = (uint16_t)((rd(v, BDA + 0x61) << 8) | rd(v, BDA + 0x60));
        return 1;
    case 0x05:
        return AL(r) == 0;
    case 0x06: case 0x07:
        scroll(v, AL(r), AH(r) == 0x06, CH(r), CL(r), DH(r), DL(r), BH(r));
        return 1;
    case 0x08:
        if (!text_mode(v) || BH(r) != 0) return 0;
        get_cursor(v, &col, &row);
        {
            uint32_t a = TEXT_BASE + (uint32_t)(row * COLS_TEXT + col) * 2;
            r->ax = (uint16_t)((rd(v, a + 1) << 8) | rd(v, a));
        }
        return 1;
    case 0x09: case 0x0A:
        if (BH(r) != 0) return 0;
        get_cursor(v, &col, &row);
        for (i = 0; i < r->cx; i++) {
            int c = col + i;
            put_cell(v, c % cols(v), row + c / cols(v), AL(r),
                     (AH(r) == 0x09 && text_mode(v)) ? BL(r) : -1, BL(r));
        }
        return 1;
    case 0x0E:
        teletype(v, AL(r), -1, BL(r));
        return 1;
    case 0x0F:
        r->ax = (uint16_t)((cols(v) << 8) | v->st.mode);
        SET_HI(r->bx, 0);
        return 1;
    case 0x10:
        return int10_palette(v, r);
    case 0x12:
        if (BL(r) != 0x10) return 0;
        r->bx = 0x0003;            /* color mode, 256 KB */
        r->cx = 0x0009;
        return 1;
    case 0x13: {
        uint32_t p = lin(r->es, r->bp);
        int save_col, save_row, attrs = AL(r) & 2;
        if (BH(r) != 0 || AL(r) > 3) return 0;
        get_cursor(v, &save_col, &save_row);
        set_cursor(v, DL(r), DH(r));
        for (i = 0; i < r->cx; i++) {
            uint8_t ch = rd(v, p++);
            uint8_t at = attrs ? rd(v, p++) : BL(r);
            if (ch == 7 || ch == 8 || ch == 10 || ch == 13) teletype(v, ch, -1, at);
            else teletype(v, ch, text_mode(v) ? at : -1, at);
        }
        if (!(AL(r) & 1)) set_cursor(v, save_col, save_row);
        return 1;
    }
    case 0x1A:
        if (AL(r) != 0) return 0;
        SET_LO(r->ax, 0x1A);
        r->bx = 0x0007;            /* VGA with analog monochrome display */
        return 1;
    default:
        return 0;
    }
}

/* ---------- ports ---------- */

static uint8_t status_3da(const vga_t *v)
{
    uint64_t t = v->st.time_ns % FRAME_NS;
    uint32_t line = (uint32_t)(t / LINE_NS);
    uint64_t in_line = t % LINE_NS;
    uint8_t s = 0;
    if (line >= 400 || in_line >= ACTIVE_NS) s |= 0x01;      /* display disabled */
    if (line >= VRETRACE_LINE && line < VRETRACE_END) s |= 0x08;
    return s;
}

int vga_port_in(vga_t *v, uint16_t port, uint8_t *value)
{
    vga_state_t *s = &v->st;
    if (port < 0x3B0 || port > 0x3DF) return VGA_PORT_NOT_MINE;
    switch (port) {
    case 0x3C7: *value = s->dac_reading ? 3 : 0; return VGA_PORT_HANDLED;
    case 0x3C8: *value = s->dac_write; return VGA_PORT_HANDLED;
    case 0x3C9:
        *value = s->dac[s->dac_read][s->dac_rcomp];
        if (++s->dac_rcomp == 3) { s->dac_rcomp = 0; s->dac_read++; }
        return VGA_PORT_HANDLED;
    case 0x3CC: *value = 0x63; return VGA_PORT_HANDLED;  /* color I/O at 3Dxh */
    case 0x3D4: *value = s->crtc_index; return VGA_PORT_HANDLED;
    case 0x3D5:
        if (s->crtc_index >= CRTC_REGS) return VGA_PORT_UNSUPPORTED;
        *value = s->crtc[s->crtc_index];
        return VGA_PORT_HANDLED;
    case 0x3DA: *value = status_3da(v); return VGA_PORT_HANDLED;
    default: return VGA_PORT_UNSUPPORTED;
    }
}

int vga_port_out(vga_t *v, uint16_t port, uint8_t value)
{
    vga_state_t *s = &v->st;
    if (port < 0x3B0 || port > 0x3DF) return VGA_PORT_NOT_MINE;
    switch (port) {
    case 0x3C7: s->dac_read = value; s->dac_rcomp = 0; s->dac_reading = 1; return VGA_PORT_HANDLED;
    case 0x3C8: s->dac_write = value; s->dac_wcomp = 0; s->dac_reading = 0; return VGA_PORT_HANDLED;
    case 0x3C9:
        s->dac_pending[s->dac_wcomp++] = (uint8_t)(value & 63);
        if (s->dac_wcomp == 3) {
            memcpy(s->dac[s->dac_write], s->dac_pending, 3);
            s->dac_wcomp = 0;
            s->dac_write++;
        }
        return VGA_PORT_HANDLED;
    case 0x3D4: s->crtc_index = (uint8_t)(value & 0x3F); return VGA_PORT_HANDLED;
    case 0x3D5:
        if (s->crtc_index >= CRTC_REGS) return VGA_PORT_UNSUPPORTED;
        s->crtc[s->crtc_index] = value;
        return VGA_PORT_HANDLED;
    default: return VGA_PORT_UNSUPPORTED;
    }
}

/* ---------- rendering ---------- */

static void render_text(const vga_t *v, const uint8_t luma[256], uint8_t *out)
{
    const vga_state_t *s = &v->st;
    uint32_t frame = vga_frame_count(v);
    int char_blink_on = ((frame >> 4) & 1) == 0;    /* 32-frame period */
    int cursor_on = ((frame >> 3) & 1) == 0;        /* 16-frame period */
    uint16_t start = (uint16_t)((s->crtc[0x0C] << 8) | s->crtc[0x0D]);
    uint16_t cpos = (uint16_t)((s->crtc[0x0E] << 8) | s->crtc[0x0F]);
    int cstart = s->crtc[0x0A] & 0x1F, cend = s->crtc[0x0B] & 0x1F;
    int cursor_cell = -1, row, col, y, x;
    uint8_t lut[16];

    for (x = 0; x < 16; x++) lut[x] = luma[s->ac_pal[x] & 0x3F];
    if (cursor_on && !(s->crtc[0x0A] & 0x20) && cstart <= cend) {
        int cell = (uint16_t)(cpos - start);
        if (cell >= 0 && cell < COLS_TEXT * ROWS) cursor_cell = cell;
    }

    for (row = 0; row < ROWS; row++) {
        for (col = 0; col < COLS_TEXT; col++) {
            int cell = row * COLS_TEXT + col;
            uint32_t a = TEXT_BASE + (((uint32_t)start * 2 + (uint32_t)cell * 2) & (TEXT_SIZE - 1));
            uint8_t ch = rd(v, a), at = rd(v, a + 1);
            uint8_t fg = lut[at & 15];
            uint8_t bg = lut[s->blink_enabled ? (at >> 4) & 7 : at >> 4];
            uint8_t glyph_fg = (s->blink_enabled && (at & 0x80) && !char_blink_on) ? bg : fg;
            for (y = 0; y < 16; y++) {
                uint8_t bits = vga_font_8x16[ch][y];
                uint8_t *o = out + (size_t)(row * 16 + y) * VGA_OUT_WIDTH + (size_t)col * 8;
                if (cell == cursor_cell && y >= cstart && y <= cend) {
                    /* the cursor is a solid bar in the cell's foreground color */
                    memset(o, fg, 8);
                    continue;
                }
                for (x = 0; x < 8; x++) o[x] = (bits & (0x80 >> x)) ? glyph_fg : bg;
            }
        }
    }
}

static void render_13h(const vga_t *v, const uint8_t luma[256], uint8_t *out)
{
    int y, x;
    for (y = 0; y < 200; y++) {
        uint8_t *o0 = out + (size_t)(y * 2) * VGA_OUT_WIDTH;
        uint8_t *o1 = o0 + VGA_OUT_WIDTH;
        const uint8_t *src = v->mem + GFX_BASE + (uint32_t)y * 320;
        for (x = 0; x < 320; x++) {
            uint8_t l = luma[src[x]];
            o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = l;
        }
    }
}

void vga_render(vga_t *v, uint8_t *out)
{
    uint8_t luma[256];
    build_luma(v, luma);
    if (v->st.mode == 0x13) render_13h(v, luma, out);
    else render_text(v, luma, out);
}

/* ---------- public API ---------- */

vga_t *vga_create(uint8_t *memory)
{
    vga_t *v;
    if (!memory) return NULL;
    v = (vga_t *)calloc(1, sizeof(vga_t));
    if (!v) return NULL;
    v->mem = memory;
    vga_reset(v);
    return v;
}

void vga_destroy(vga_t *v) { free(v); }

void vga_reset(vga_t *v)
{
    memset(&v->st, 0, sizeof(v->st));
    set_mode(v, 0x03, 1);
}

void vga_advance(vga_t *v, uint32_t microseconds) { v->st.time_ns += (uint64_t)microseconds * 1000u; }
uint32_t vga_frame_count(const vga_t *v) { return (uint32_t)(v->st.time_ns / FRAME_NS); }
uint8_t vga_mode(const vga_t *v) { return v->st.mode; }

size_t vga_state_size(void) { return sizeof(vga_state_t); }

size_t vga_save_state(const vga_t *v, void *buf, size_t len)
{
    if (len < sizeof(vga_state_t)) return 0;
    memcpy(buf, &v->st, sizeof(vga_state_t));
    return sizeof(vga_state_t);
}

int vga_load_state(vga_t *v, const void *buf, size_t len)
{
    if (len != sizeof(vga_state_t)) return -1;
    memcpy(&v->st, buf, sizeof(vga_state_t));
    return 0;
}

/* ---------- phosphor persistence ---------- */

struct vga_phosphor {
    float half_life_ms;
    float level[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];
};

vga_phosphor_t *vga_phosphor_create(float half_life_ms)
{
    vga_phosphor_t *p = (vga_phosphor_t *)calloc(1, sizeof(vga_phosphor_t));
    if (p) p->half_life_ms = half_life_ms > 0.0f ? half_life_ms : 1.0f;
    return p;
}

void vga_phosphor_destroy(vga_phosphor_t *p) { free(p); }

void vga_phosphor_apply(vga_phosphor_t *p, const uint8_t *in, float dt_seconds, uint8_t *out)
{
    float decay = (float)pow(0.5, (double)dt_seconds * 1000.0 / (double)p->half_life_ms);
    size_t i;
    for (i = 0; i < (size_t)VGA_OUT_WIDTH * VGA_OUT_HEIGHT; i++) {
        float lv = p->level[i] * decay;
        if ((float)in[i] > lv) lv = (float)in[i];
        p->level[i] = lv;
        out[i] = (uint8_t)(lv + 0.5f);
    }
}
