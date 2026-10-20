/* Unit tests for vga86. They drive the device directly (no CPU needed). */
#include "vga.h"
#include "../src/font_cp437.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

static uint8_t mem[1 << 20];
static uint8_t px[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];

static int int10(vga_t *v, uint16_t ax, uint16_t bx, uint16_t cx, uint16_t dx, vga_bios_regs_t *out)
{
    vga_bios_regs_t r;
    int ok;
    r.ax = ax; r.bx = bx; r.cx = cx; r.dx = dx; r.es = 0; r.bp = 0;
    ok = vga_int10(v, &r);
    if (out) *out = r;
    return ok;
}

static uint8_t P(int x, int y) { return px[y * VGA_OUT_WIDTH + x]; }

/* Does the 8x16 cell at (col,row) show glyph ch with colors fg/bg? */
static int cell_is(int col, int row, uint8_t ch, uint8_t fg, uint8_t bg)
{
    int x, y;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 8; x++) {
            uint8_t want = (vga_font_8x16[ch][y] & (0x80 >> x)) ? fg : bg;
            if (P(col * 8 + x, row * 16 + y) != want) return 0;
        }
    return 1;
}

/* Luminance a monochrome monitor shows for 6-bit RGB */
static uint8_t L(int r, int g, int b)
{
    int l6 = (30 * r + 59 * g + 11 * b + 50) / 100;
    return (uint8_t)((l6 * 255 + 31) / 63);
}

/* 3DAh status read by a fresh device after us microseconds */
static uint8_t status_at(double us)
{
    static uint8_t scratch[1 << 20];
    vga_t *t = vga_create(scratch);
    uint8_t s = 0xFF;
    vga_advance(t, (uint32_t)us);
    vga_port_in(t, 0x3DA, &s);
    vga_destroy(t);
    return s;
}

static void advance_frames(vga_t *v, int frames) { vga_advance(v, (uint32_t)frames * VGA_FRAME_US); }

int main(void)
{
    vga_t *v = vga_create(mem);
    vga_bios_regs_t r;
    uint8_t b;
    const uint8_t gray = L(42, 42, 42), white = L(63, 63, 63);

    /* --- power-on state --- */
    CHECK(vga_mode(v) == 0x03, "power-on mode is 03h");
    CHECK(mem[0x449] == 0x03 && mem[0x44A] == 80 && mem[0x484] == 24 && mem[0x485] == 16,
          "BIOS data area: mode, columns, rows-1, character height");
    CHECK(mem[0xB8000] == ' ' && mem[0xB8001] == 0x07 && mem[0xB8000 + 3998] == ' ', "screen cleared to spaces, attribute 07h");

    /* --- teletype output --- */
    int10(v, 0x0E00 | 'H', 0, 0, 0, NULL);
    int10(v, 0x0E00 | 'i', 0, 0, 0, NULL);
    CHECK(mem[0xB8000] == 'H' && mem[0xB8002] == 'i' && mem[0xB8001] == 0x07, "teletype writes into B800:0000 and keeps the attribute");
    int10(v, 0x0300, 0, 0, 0, &r);
    CHECK(r.dx == 0x0002, "cursor advanced to column 2 (INT 10h AH=03h)");
    CHECK(mem[0x450] == 2 && mem[0x451] == 0, "cursor position stored in the BIOS data area");

    advance_frames(v, 0);
    vga_render(v, px);
    CHECK(cell_is(0, 0, 'H', gray, 0) && cell_is(1, 0, 'i', gray, 0), "rendered glyphs match the font, light gray on black");
    CHECK(L(42, 42, 42) == 170 && white == 255, "luminance uses 30/59/11 summing (gray 42 -> 170)");

    /* cursor: scanlines 13-14 of the cursor cell, visible in the first 8 frames */
    CHECK(P(16, 13) == gray && P(23, 14) == gray && P(16, 12) == 0 && P(16, 15) == 0, "hardware cursor bar on scanlines 13-14");
    advance_frames(v, 8);
    vga_render(v, px);
    CHECK(P(16, 13) == 0, "cursor blinks off after 8 frames");
    advance_frames(v, 8);

    /* --- CR, LF and scrolling --- */
    {
        int i;
        int10(v, 0x0200, 0, 0, 0x1800, NULL);   /* row 24, col 0 */
        int10(v, 0x0E00 | 'X', 0, 0, 0, NULL);
        int10(v, 0x0E0D, 0, 0, 0, NULL);
        int10(v, 0x0E0A, 0, 0, 0, NULL);        /* LF on the last row scrolls */
        CHECK(mem[0xB8000 + 23 * 160] == 'X' && mem[0xB8000 + 24 * 160] == ' ', "LF on the last line scrolls the screen up");
        CHECK(mem[0xB8000] == ' ', "the first line scrolled away");
        int10(v, 0x0300, 0, 0, 0, &r);
        CHECK(r.dx == 0x1800, "cursor stays on the last row after scrolling");
        for (i = 0; i < 81; i++) int10(v, 0x0E00 | 'w', 0, 0, 0, NULL);
        int10(v, 0x0300, 0, 0, 0, &r);
        CHECK(r.dx == 0x1801, "writing past column 79 wraps (and scrolls)");
    }

    /* --- write char/attribute, read back, scroll window --- */
    int10(v, 0x0200, 0, 0, 0x0505, NULL);
    int10(v, 0x0900 | 'A', 0x001F, 3, 0, NULL);
    CHECK(mem[0xB8000 + (5 * 80 + 5) * 2] == 'A' && mem[0xB8000 + (5 * 80 + 7) * 2 + 1] == 0x1F, "AH=09h writes CX copies with attribute");
    int10(v, 0x0800, 0, 0, 0, &r);
    CHECK(r.ax == 0x1F41, "AH=08h reads character and attribute at the cursor");
    int10(v, 0x0601, 0x7000, 0x0505, 0x0607, NULL);   /* scroll window rows 5-6, cols 5-7 up by 1 */
    CHECK(mem[0xB8000 + (5 * 80 + 5) * 2] == ' ' && mem[0xB8000 + (6 * 80 + 5) * 2 + 1] == 0x70, "AH=06h scrolls a window and fills with BH");

    /* --- write string with attributes (AH=13h, AL=3) --- */
    {
        static const uint8_t s[] = {'O', 0x70, 'K', 0x0F};
        memcpy(mem + 0x20000, s, sizeof(s));
        r.ax = 0x1303; r.bx = 0; r.cx = 2; r.dx = 0x0A00; r.es = 0x2000; r.bp = 0;
        CHECK(vga_int10(v, &r) == 1, "AH=13h accepted");
        CHECK(mem[0xB8000 + 10 * 160] == 'O' && mem[0xB8000 + 10 * 160 + 1] == 0x70 && mem[0xB8000 + 10 * 160 + 3] == 0x0F,
              "AH=13h writes characters with their own attributes");
        int10(v, 0x0300, 0, 0, 0, &r);
        CHECK(r.dx == 0x0A02, "AH=13h AL bit 0 moves the cursor");
    }

    /* --- attributes: intensity, background, blink --- */
    int10(v, 0x0003, 0, 0, 0, NULL);
    int10(v, 0x0200, 0, 0, 0x184F, NULL);           /* cursor out of the way (row 24, col 79) */
    mem[0xB8000] = 'B'; mem[0xB8001] = 0x8F;        /* blinking bright white */
    mem[0xB8002] = 'b'; mem[0xB8003] = 0x70;        /* black on light gray */
    {
        uint32_t f = vga_frame_count(v);
        advance_frames(v, (int)((32 - f % 32) % 32));   /* align to a blink-on phase */
    }
    vga_render(v, px);
    CHECK(cell_is(0, 0, 'B', white, 0), "blink attribute: visible in the on phase");
    CHECK(cell_is(1, 0, 'b', 0, gray), "background color from the attribute high nibble");
    advance_frames(v, 16);
    vga_render(v, px);
    CHECK(cell_is(0, 0, 'B', 0, 0), "blink attribute: hidden in the off phase");
    int10(v, 0x1003, 0x0000, 0, 0, NULL);           /* blink off: bit 7 = bright background */
    vga_render(v, px);
    CHECK(cell_is(0, 0, 'B', white, L(21, 21, 21)), "AX=1003h BL=0 turns bit 7 into background intensity");

    /* --- CRTC: cursor off, start address --- */
    vga_port_out(v, 0x3D4, 0x0A);
    vga_port_in(v, 0x3D5, &b);
    CHECK(b == 0x0D, "CRTC register 0Ah readable through 3D4h/3D5h");
    vga_port_out(v, 0x3D5, 0x20);
    {
        uint32_t f = vga_frame_count(v);
        advance_frames(v, (int)((16 - f % 16) % 16));
    }
    int10(v, 0x0200, 0, 0, 0x0300, NULL);
    vga_render(v, px);
    CHECK(P(0, 3 * 16 + 13) == 0, "CRTC 0Ah bit 5 disables the cursor");
    mem[0xB8000 + 160] = 'S'; mem[0xB8000 + 161] = 0x07;   /* first cell of row 1 */
    vga_port_out(v, 0x3D4, 0x0C); vga_port_out(v, 0x3D5, 0);
    vga_port_out(v, 0x3D4, 0x0D); vga_port_out(v, 0x3D5, 80);
    vga_render(v, px);
    CHECK(cell_is(0, 0, 'S', gray, 0), "CRTC start address scrolls the display (hardware scrolling)");
    vga_port_out(v, 0x3D4, 0x0D); vga_port_out(v, 0x3D5, 0);

    /* --- mode 13h --- */
    memset(mem + 0xA0000, 0xAA, 0x10000);
    int10(v, 0x0013, 0, 0, 0, NULL);
    CHECK(vga_mode(v) == 0x13 && mem[0x449] == 0x13 && mem[0x44A] == 40, "INT 10h AX=0013h sets mode 13h");
    CHECK(mem[0xA0000] == 0 && mem[0xAFFFF] == 0, "mode set clears A000:0000-FFFF");
    mem[0xA0000 + 10 * 320 + 10] = 15;
    vga_render(v, px);
    CHECK(P(20, 20) == white && P(21, 20) == white && P(20, 21) == white && P(21, 21) == white && P(22, 20) == 0 && P(19, 21) == 0,
          "mode 13h pixels are doubled to 2x2");

    /* default palette: EGA colors, grays, then the hue rings */
    vga_port_out(v, 0x3C7, 16 + 8);
    { uint8_t rr, gg, bb; vga_port_in(v, 0x3C9, &rr); vga_port_in(v, 0x3C9, &gg); vga_port_in(v, 0x3C9, &bb);
      CHECK(rr == 28 && gg == 28 && bb == 28, "default palette entry 24 is gray 28"); }
    vga_port_out(v, 0x3C7, 32);
    { uint8_t rr, gg, bb; vga_port_in(v, 0x3C9, &rr); vga_port_in(v, 0x3C9, &gg); vga_port_in(v, 0x3C9, &bb);
      CHECK(rr == 0 && gg == 0 && bb == 63, "default palette entry 32 is bright blue"); }
    vga_port_out(v, 0x3C7, 40);
    { uint8_t rr, gg, bb; vga_port_in(v, 0x3C9, &rr); vga_port_in(v, 0x3C9, &gg); vga_port_in(v, 0x3C9, &bb);
      CHECK(rr == 63 && gg == 0 && bb == 0, "default palette entry 40 is bright red"); }

    /* DAC through ports */
    vga_port_out(v, 0x3C8, 200);
    vga_port_out(v, 0x3C9, 63); vga_port_out(v, 0x3C9, 0); vga_port_out(v, 0x3C9, 0);
    vga_port_out(v, 0x3C9, 0); vga_port_out(v, 0x3C9, 63); vga_port_out(v, 0x3C9, 0);   /* auto-increment to 201 */
    vga_port_in(v, 0x3C8, &b);
    CHECK(b == 202, "DAC write index auto-increments after each RGB triple");
    mem[0xA0000] = 200; mem[0xA0001] = 201;
    vga_render(v, px);
    CHECK(P(0, 0) == L(63, 0, 0) && P(2, 0) == L(0, 63, 0), "pure red and green show as different grays (30% vs 59%)");
    vga_port_out(v, 0x3C7, 201);
    { uint8_t rr, gg, bb; vga_port_in(v, 0x3C9, &rr); vga_port_in(v, 0x3C9, &gg); vga_port_in(v, 0x3C9, &bb);
      CHECK(rr == 0 && gg == 63 && bb == 0, "DAC read back through 3C7h/3C9h"); }
    vga_port_in(v, 0x3C7, &b);
    CHECK(b == 3, "3C7h reports read mode");

    /* BIOS palette functions */
    int10(v, 0x1010, 0x0005, 0x0102, 0x0300, NULL);      /* entry 5 = (3,1,2) */
    int10(v, 0x1015, 0x0005, 0, 0, &r);
    CHECK((r.dx >> 8) == 3 && (r.cx >> 8) == 1 && (r.cx & 0xFF) == 2, "AX=1010h / 1015h set and read one DAC entry");
    int10(v, 0x101B, 200, 1, 0, NULL);
    vga_port_out(v, 0x3C7, 200);
    { uint8_t rr, gg, bb; vga_port_in(v, 0x3C9, &rr); vga_port_in(v, 0x3C9, &gg); vga_port_in(v, 0x3C9, &bb);
      CHECK(rr == 19 && gg == 19 && bb == 19, "AX=101Bh sums colors to gray (red 63 -> 19)"); }

    /* text in mode 13h uses the 8x8 font in color BL */
    int10(v, 0x0200, 0, 0, 0x0000, NULL);
    int10(v, 0x0E00 | 'A', 0x000F, 0, 0, NULL);
    {
        int x, y, okk = 1;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                if ((mem[0xA0000 + y * 320 + x] == 15) != !!(vga_font_8x8['A'][y] & (0x80 >> x))) okk = 0;
        CHECK(okk, "teletype in mode 13h draws the 8x8 glyph in color BL");
    }
    int10(v, 0x0F00, 0, 0, 0, &r);
    CHECK(r.ax == 0x2813, "AH=0Fh reports mode 13h and 40 columns");

    /* --- retrace timing on 3DAh --- */
    {
        /* one scanline is 14268 / 449 = 31.78 us; 25.42 us of it is visible */
        CHECK(status_at(1) == 0x00, "3DAh at the start of a visible line: display enabled, no retrace");
        CHECK(status_at(10 * 31.777 + 28) == 0x01, "3DAh in horizontal blanking of a visible line: bit 0 only");
        CHECK(status_at(405 * 31.777 + 5) == 0x01, "3DAh in vertical blanking before retrace: bit 0 only");
        CHECK(status_at(412 * 31.777 + 10) == 0x09, "3DAh during vertical retrace (line 412): bits 3 and 0");
        CHECK(status_at(413 * 31.777 + 20) == 0x09, "retrace lasts two lines (412-413)");
        CHECK(status_at(414 * 31.777 + 5) == 0x01, "after retrace: blanking only");
        CHECK(status_at(VGA_FRAME_US + 412 * 31.777 + 10) == 0x09, "retrace repeats every frame");
        {
            vga_t *t = vga_create(mem);
            vga_advance(t, VGA_FRAME_US * 3 - 1);
            CHECK(vga_frame_count(t) == 2, "frame counter: 2 frames just before 3 * 14268 us");
            vga_advance(t, 1);
            CHECK(vga_frame_count(t) == 3, "frame counter: 3 frames at 3 * 14268 us (70 Hz)");
            vga_destroy(t);
        }
        vga_reset(v);
    }

    /* --- unsupported things are reported --- */
    CHECK(int10(v, 0x0012, 0, 0, 0, NULL) == 0, "unsupported video mode (12h) is reported");
    CHECK(int10(v, 0x1100, 0, 0, 0, NULL) == 0, "unsupported BIOS function (AH=11h, fonts) is reported");
    CHECK(int10(v, 0x0201, 0x0100, 0, 0, NULL) == 0, "display pages other than 0 are reported as unsupported");
    CHECK(vga_port_out(v, 0x3C4, 1) == VGA_PORT_UNSUPPORTED, "sequencer port 3C4h reported as unsupported");
    CHECK(vga_port_out(v, 0x3C0, 1) == VGA_PORT_UNSUPPORTED, "attribute controller port 3C0h reported as unsupported");
    CHECK(vga_port_out(v, 0x60, 1) == VGA_PORT_NOT_MINE && vga_port_in(v, 0x61, &b) == VGA_PORT_NOT_MINE, "non-video ports are left to other devices");
    CHECK(vga_mode(v) == 0x03, "a rejected mode set leaves the mode unchanged");

    /* --- identification --- */
    int10(v, 0x1A00, 0, 0, 0, &r);
    CHECK((r.ax & 0xFF) == 0x1A && (r.bx & 0xFF) == 0x07, "AX=1A00h identifies a VGA with monochrome analog display");

    /* --- save / load state --- */
    {
        size_t n = vga_state_size();
        void *buf = malloc(n);
        static uint8_t a[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];
        vga_t *w = vga_create(mem);
        int10(v, 0x0013, 0, 0, 0, NULL);
        vga_port_out(v, 0x3C8, 1);
        vga_port_out(v, 0x3C9, 10); vga_port_out(v, 0x3C9, 20); vga_port_out(v, 0x3C9, 30);
        mem[0xA0000 + 5] = 1;
        vga_advance(v, 123456);
        CHECK(vga_save_state(v, buf, n) == n && vga_load_state(w, buf, n) == 0, "state saved and loaded");
        vga_render(v, a);
        vga_render(w, px);
        CHECK(memcmp(a, px, sizeof(a)) == 0 && vga_frame_count(w) == vga_frame_count(v), "restored device renders identically");
        free(buf);
        vga_destroy(w);
    }

    /* --- phosphor persistence --- */
    {
        vga_phosphor_t *ph = vga_phosphor_create(20.0f);
        static uint8_t in[VGA_OUT_WIDTH * VGA_OUT_HEIGHT], out[VGA_OUT_WIDTH * VGA_OUT_HEIGHT];
        in[0] = 200;
        vga_phosphor_apply(ph, in, 0.016f, out);
        CHECK(out[0] == 200, "phosphor: a lit pixel shows at full brightness at once");
        in[0] = 0;
        vga_phosphor_apply(ph, in, 0.020f, out);
        CHECK(out[0] == 100, "phosphor: after one half-life it shows at half brightness");
        vga_phosphor_apply(ph, in, 0.020f, out);
        CHECK(out[0] == 50, "phosphor: and a quarter after two");
        vga_phosphor_destroy(ph);
    }

    /* --- font sanity --- */
    {
        int i, blank = 0;
        for (i = 1; i < 256; i++) {
            int y, any = 0;
            if (i == 0x20 || i == 0xFF) continue;   /* space and non-breaking space */
            for (y = 0; y < 16; y++) any |= vga_font_8x16[i][y];
            if (!any) blank++;
        }
        CHECK(blank == 0, "every visible CP437 character has a glyph");
    }

    vga_destroy(v);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
