/*
 * Machine tests for pc86: a whole PC (CPU, board chips, VGA card, monitor,
 * keyboard) running small programs.
 */
#include "asm86.h"
#include "fat12.h"
#include "pc.h"
#include "pc_fdc.h"
#include "pc_vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok   %s\n", msg); } while (0)

#define FRAME (1.0 / 60.0)

static pc_t *pc;
static pc_vga_card_t *vga;
static monitor_t *mono, *color, *cga;
static kbd_t *kbd;
static uint8_t screen[640 * 400 * 3];

/* Minimal DOS: INT 20h and INT 21h AH=4Ch end the program. */
static int dos(void *user, pc_t *p, uint8_t n)
{
    vm86_regs_t *r = vm86_regs(pc_cpu(p));
    (void)user;
    if (n == 0x20 || (n == 0x21 && (r->ax >> 8) == 0x4C)) { pc_stop(p); return 1; }
    return 0;
}

static asm86_symbol_t syms[256];
static int nsyms;

/* Assembles and loads a program; keeps its symbols for sym(). */
static void load(const char *src)
{
    asm86_result_t a;
    int i;
    if (asm86_assemble(src, &a) != 0) {
        printf("assembly error line %d: %s\n", a.errors[0].line, a.errors[0].message);
        exit(1);
    }
    nsyms = a.symbol_count < 256 ? a.symbol_count : 256;
    for (i = 0; i < nsyms; i++) syms[i] = a.symbols[i];
    if (pc_load_com(pc, a.code, a.size) != 0) { printf("cannot load program\n"); exit(1); }
    asm86_free(&a);
}

static uint16_t var(const char *name)
{
    int i;
    uint8_t *m = pc_memory(pc) + (PC_COM_SEGMENT << 4);
    for (i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, name)) return (uint16_t)(m[syms[i].value] | m[syms[i].value + 1] << 8);
    printf("no symbol %s\n", name);
    exit(1);
}

/* Runs up to max_frames engine frames; returns how many ran until it stopped. */
static int run_frames(int max_frames)
{
    int f;
    for (f = 0; f < max_frames; f++) {
        pc_status_t st = pc_status(pc);
        if (st != PC_RUNNING && st != PC_HALTED) break;
        pc_run(pc, FRAME);
    }
    return f;
}

static void boot(void)
{
    pc_power_off(pc);
    pc_power_on(pc);
    run_frames(3);
}

static void type(uint16_t key)
{
    kbd_key(kbd, key, 1); run_frames(2);
    kbd_key(kbd, key, 0); run_frames(2);
}

static int text_at(int col, int row, const char *s)
{
    const uint8_t *m = pc_memory(pc) + 0xB8000 + (row * 80 + col) * 2;
    for (; *s; s++, m += 2) if (*m != (uint8_t)*s) return 0;
    return 1;
}

/* How many times s is on the text screen. */
static int on_screen(const char *s)
{
    int row, col, n = 0;
    for (row = 0; row < 25; row++)
        for (col = 0; col + (int)strlen(s) <= 80; col++)
            n += text_at(col, row, s);
    return n;
}

/* A boot sector that reads with the BIOS and then drives the floppy
 * controller and the DMA controller itself, the way a disk driver does. */
static const char *BOOT_SECTOR =
    "org 7C00h\n"
    "    jmp short start\n"
    "    nop\n"
    "    times 59 db 0\n"                       /* the BPB stays here */
    "start:\n"
    "    xor ax, ax\n"
    "    mov ds, ax\n"
    "    mov es, ax\n"
    "    mov [drive], dl\n"
    "    mov ax, 0201h\n"                       /* BIOS: cylinder 0, sector 2 to 0000:8000 */
    "    mov bx, 8000h\n"
    "    mov cx, 0002h\n"
    "    xor dh, dh\n"
    "    int 13h\n"
    "    mov [bios_status], ah\n"
    "    mov al, 06h\n"                         /* DMA channel 2: mask, */
    "    out 0Ah, al\n"
    "    mov al, 46h\n"                         /* single transfer, write to memory, */
    "    out 0Bh, al\n"
    "    out 0Ch, al\n"
    "    xor al, al\n"                          /* address 0000:9000, */
    "    out 04h, al\n"
    "    mov al, 90h\n"
    "    out 04h, al\n"
    "    xor al, al\n"
    "    out 81h, al\n"
    "    mov al, 0FFh\n"                        /* 512 bytes, */
    "    out 05h, al\n"
    "    mov al, 01h\n"
    "    out 05h, al\n"
    "    mov al, 02h\n"                         /* unmask */
    "    out 0Ah, al\n"
    "    mov dx, 3F2h\n"                        /* motor A, DMA and IRQ on */
    "    mov al, 1Ch\n"
    "    out dx, al\n"
    "    and byte [43Eh], 7Fh\n"
    "    mov al, 0Fh\n"                         /* SEEK drive 0 to cylinder 1 */
    "    call send\n"
    "    xor al, al\n"
    "    call send\n"
    "    mov al, 1\n"
    "    call send\n"
    "    call wait_irq\n"
    "    mov al, 08h\n"                         /* SENSE INTERRUPT STATUS */
    "    call send\n"
    "    call recv\n"
    "    mov [seek_st0], al\n"
    "    call recv\n"
    "    mov [seek_pcn], al\n"
    "    mov si, read_cmd\n"                    /* READ DATA cylinder 1, head 0, sector 3 */
    "    mov cx, 9\n"
    "next_byte:\n"
    "    lodsb\n"
    "    call send\n"
    "    loop next_byte\n"
    "    call wait_irq\n"
    "    mov di, result\n"
    "    mov cx, 7\n"
    "next_result:\n"
    "    call recv\n"
    "    stosb\n"
    "    loop next_result\n"
    "    mov byte [done], 1\n"
    "halt:\n"
    "    hlt\n"
    "    jmp halt\n"
    "send:\n"
    "    push ax\n"
    "    mov dx, 3F4h\n"
    "send_wait:\n"
    "    in al, dx\n"
    "    and al, 0C0h\n"
    "    cmp al, 80h\n"
    "    jne send_wait\n"
    "    pop ax\n"
    "    mov dx, 3F5h\n"
    "    out dx, al\n"
    "    ret\n"
    "recv:\n"
    "    mov dx, 3F4h\n"
    "recv_wait:\n"
    "    in al, dx\n"
    "    and al, 0C0h\n"
    "    cmp al, 0C0h\n"
    "    jne recv_wait\n"
    "    mov dx, 3F5h\n"
    "    in al, dx\n"
    "    ret\n"
    "wait_irq:\n"                               /* the BIOS's INT 0Eh sets bit 7 of 0040:003E */
    "    sti\n"
    "    hlt\n"
    "    test byte [43Eh], 80h\n"
    "    jz wait_irq\n"
    "    and byte [43Eh], 7Fh\n"
    "    ret\n"
    "read_cmd db 0E6h, 0, 1, 0, 3, 2, 18, 1Bh, 0FFh\n"
    "drive db 0FFh\n"
    "bios_status db 0FFh\n"
    "seek_st0 db 0\n"
    "seek_pcn db 0\n"
    "done db 0\n"
    "result times 7 db 0\n";

static uint32_t boot_sym(const asm86_result_t *a, const char *name)
{
    int i;
    for (i = 0; i < a->symbol_count; i++)
        if (!strcmp(a->symbols[i].name, name)) return (uint32_t)a->symbols[i].value;
    printf("no symbol %s\n", name);
    exit(1);
}

int main(void)
{
    uint8_t *state;
    size_t len;
    int f;

    pc = pc_create();
    vga = pc_vga_card_create();
    mono = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_MONO));
    color = monitor_create(monitor_builtin_model(MONITOR_MODEL_VGA_COLOR));
    cga = monitor_create(monitor_builtin_model(MONITOR_MODEL_CGA_COLOR));
    kbd = kbd_create(kbd_builtin_model(KBD_MODEL_ENHANCED101));
    CHECK(pc && vga && mono && kbd, "machine, card, monitor and keyboard created");
    pc_set_int_hook(pc, dos, NULL);

    /* ---------- assembling the machine ---------- */
    CHECK(pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == 0, "VGA card inserted in slot 0");
    CHECK(pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == -1, "a slot holds one card");
    CHECK(pc_vga_card_connect_monitor(vga, cga) == -1, "a CGA monitor does not fit the VGA connector");
    CHECK(pc_vga_card_connect_monitor(vga, mono) == 0, "a mono VGA monitor does");
    pc_connect_keyboard(pc, kbd);
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_NO_SIGNAL, "machine off: the monitor has no signal");

    /* ---------- power on ---------- */
    pc_power_on(pc);
    CHECK(pc_insert_card(pc, 1, pc_vga_card_slot(vga)) == -1 && pc_remove_card(pc, 0) == NULL,
          "cards cannot be inserted or removed with the power on");
    CHECK(kbd_powered(kbd) && kbd_leds(kbd) == KBD_LED_NUM, "the keyboard gets power; the POST lights Num Lock");
    run_frames(6);
    CHECK(pc_status(pc) == PC_HALTED && text_at(0, 0, "No boot device available."),
          "with no storage the BIOS says so and waits in HLT");
    {
        int lit = 0, i;
        CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_SHOWING, "the monitor is showing the card's signal");
        for (i = 0; i < 640 * 16; i++) lit += screen[i] > 100;
        CHECK(lit > 200, "and the first text row has the BIOS message on it");
    }
    {
        uint8_t *m = pc_memory(pc);
        uint32_t t0 = m[0x46C] | m[0x46D] << 8;
        run_frames(60);
        f = (int)((m[0x46C] | m[0x46D] << 8) - t0);
        CHECK(f == 18 || f == 19, "the BIOS timer interrupt counts 18.2 ticks per second while idle");
    }

    /* ---------- a program hooking the user timer interrupt ---------- */
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:1Ch*4], tick\n"
         "    mov [es:1Ch*4+2], cs\n"
         "    sti\n"
         "wait_ticks:\n"
         "    hlt\n"
         "    cmp word [count], 18\n"
         "    jb wait_ticks\n"
         "    int 20h\n"
         "tick:\n"
         "    inc word [cs:count]\n"
         "    iret\n"
         "count dw 0\n");
    f = run_frames(120);
    CHECK(pc_status(pc) == PC_STOPPED && var("count") == 18 && f >= 58 && f <= 61,
          "INT 1Ch hook: 18 ticks take one second of emulated time");

    /* ---------- reprogramming the PIT ---------- */
    boot();
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:8*4], tick\n"
         "    mov [es:8*4+2], cs\n"
         "    mov al, 34h\n"
         "    out 43h, al\n"
         "    mov ax, 1193\n"
         "    out 40h, al\n"
         "    mov al, ah\n"
         "    out 40h, al\n"
         "    sti\n"
         "wait_ticks:\n"
         "    hlt\n"
         "    cmp word [count], 1000\n"
         "    jb wait_ticks\n"
         "    int 20h\n"
         "tick:\n"
         "    push ax\n"
         "    inc word [cs:count]\n"
         "    mov al, 20h\n"
         "    out 20h, al\n"
         "    pop ax\n"
         "    iret\n"
         "count dw 0\n");
    f = run_frames(120);
    CHECK(pc_status(pc) == PC_STOPPED && f >= 59 && f <= 61, "own INT 08h with the PIT at 1000 Hz: 1000 ticks in one second");

    /* ---------- BIOS services ---------- */
    boot();
    run_frames(30);
    load("org 100h\n"
         "    int 12h\n"
         "    mov [mem], ax\n"
         "    xor ah, ah\n"
         "    int 1Ah\n"
         "    mov [ticks], dx\n"
         "    mov ax, 1A00h\n"
         "    int 10h\n"
         "    mov [dcc], bx\n"
         "    int 20h\n"
         "mem dw 0\n"
         "ticks dw 0\n"
         "dcc dw 0\n");
    run_frames(2);
    CHECK(var("mem") == 640, "INT 12h: 640 KB");
    CHECK(var("ticks") >= 9 && var("ticks") <= 11, "INT 1Ah AH=00h: the tick count since power on");
    CHECK((var("dcc") & 0xFF) == 0x07, "INT 10h AX=1A00h: monochrome VGA display (the monitor's ID pins)");
    pc_vga_card_connect_monitor(vga, color);
    load("org 100h\n"
         "    mov ax, 1A00h\n"
         "    int 10h\n"
         "    mov [dcc], bx\n"
         "    int 20h\n"
         "dcc dw 0\n");
    run_frames(2);
    CHECK((var("dcc") & 0xFF) == 0x08, "after hot-swapping to a color monitor: color VGA display");
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == MONITOR_SHOWING, "and the color monitor shows the picture");
    pc_vga_card_connect_monitor(vga, NULL);
    CHECK(pc_vga_card_display(vga, (float)FRAME, screen) == -1, "with the monitor unplugged there is nothing to show");
    pc_vga_card_connect_monitor(vga, mono);

    /* ---------- keyboard ---------- */
    load("org 100h\n"
         "    xor ah, ah\n"
         "    int 16h\n"
         "    mov [key1], ax\n"
         "    xor ah, ah\n"
         "    int 16h\n"
         "    mov [key2], ax\n"
         "    int 20h\n"
         "key1 dw 0\n"
         "key2 dw 0\n");
    run_frames(3);
    type(0x1E);
    CHECK(var("key1") == 0x1E61, "INT 16h gets 'a' from the keyboard");
    pc_connect_keyboard(pc, NULL);
    CHECK(!kbd_powered(kbd), "unplugging the keyboard cuts its power");
    type(0x30);
    CHECK(pc_status(pc) == PC_RUNNING && var("key2") == 0, "keys on an unplugged keyboard go nowhere");
    pc_connect_keyboard(pc, kbd);
    run_frames(3);
    type(0x30);
    CHECK(pc_status(pc) == PC_STOPPED && var("key2") == 0x3062, "plugged back in while running: 'b' arrives");

    pc_connect_keyboard(pc, NULL);
    load("org 100h\n"
         "    mov al, 0EDh\n"
         "    out 60h, al\n"
         "    mov cx, 100\n"
         "d1: loop d1\n"
         "    in al, 64h\n"
         "    mov [status], al\n"
         "    int 20h\n"
         "status db 0, 0\n");
    run_frames(2);
    CHECK((var("status") & 0x40) != 0, "a command with no keyboard times out (port 64h bit 6)");
    pc_connect_keyboard(pc, kbd);

    /* ---------- port 61h: speaker and refresh ---------- */
    load("org 100h\n"
         "    mov al, 0B6h\n"
         "    out 43h, al\n"
         "    mov ax, 1193\n"
         "    out 42h, al\n"
         "    mov al, ah\n"
         "    out 42h, al\n"
         "    in al, 61h\n"
         "    or al, 3\n"
         "    out 61h, al\n"
         "    mov cx, 200\n"
         "    in al, 61h\n"
         "    and al, 10h\n"
         "    mov bl, al\n"
         "poll:\n"
         "    in al, 61h\n"
         "    and al, 10h\n"
         "    cmp al, bl\n"
         "    je same\n"
         "    inc word [toggles]\n"
         "    mov bl, al\n"
         "same:\n"
         "    loop poll\n"
         "    hlt\n"
         "    jmp $-1\n"
         "toggles dw 0\n");
    run_frames(3);
    CHECK(pc_speaker_hz(pc) > 999.0 && pc_speaker_hz(pc) < 1001.0, "the speaker plays the 1000 Hz tone of PIT channel 2");
    CHECK(var("toggles") > 10, "port 61h bit 4 toggles with the DRAM refresh");

    /* ---------- the bus ---------- */
    load("org 100h\n"
         "    mov dx, 378h\n"
         "    in al, dx\n"
         "    mov [v], al\n"
         "    out dx, al\n"
         "    int 20h\n"
         "v db 0, 0\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_STOPPED && (var("v") & 0xFF) == 0xFF, "a port nobody decodes reads FFh (no printer card)");
    load("org 100h\n"
         "    mov dx, 3C4h\n"
         "    out dx, al\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_ERROR && strstr(pc_error(pc), "3C4") != NULL, "a VGA port the card does not emulate is reported");
    printf("     (%s)\n", pc_error(pc));
    boot();
    load("org 100h\n"
         "    mov ax, 4F00h\n"
         "    int 10h\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_ERROR && strstr(pc_error(pc), "INT 10h") != NULL, "an unsupported video BIOS call is reported");
    printf("     (%s)\n", pc_error(pc));

    /* ---------- save and load ---------- */
    boot();
    load("org 100h\n"
         "    xor ax, ax\n"
         "    mov es, ax\n"
         "    cli\n"
         "    mov word [es:1Ch*4], tick\n"
         "    mov [es:1Ch*4+2], cs\n"
         "    sti\n"
         "forever:\n"
         "    hlt\n"
         "    jmp forever\n"
         "tick:\n"
         "    inc word [cs:count]\n"
         "    iret\n"
         "count dw 0\n");
    run_frames(30);
    len = pc_state_size(pc);
    state = (uint8_t *)malloc(len);
    {
        uint16_t saved = var("count");
        CHECK(pc_save_state(pc, state, len) == len, "machine state saved");
        run_frames(60);
        CHECK(var("count") > saved && pc_load_state(pc, state, len) == 0 && var("count") == saved, "and loaded back");
        run_frames(60);
        CHECK(var("count") >= saved + 18, "it keeps running from there");
    }
    free(state);

    /* ---------- diskettes ---------- */
    {
        pc_fdc_card_t *fdc = pc_fdc_card_create();
        fdd_t *drive = fdd_create(fdd_builtin_model(FDD_MODEL_35_HD));
        disk_t *disk = disk_create(DISK_1440K);
        uint8_t *m = pc_memory(pc), *img;
        asm86_result_t a;

        pc_power_off(pc);
        CHECK(pc_insert_card(pc, 1, pc_fdc_card_slot(fdc)) == 0, "diskette adapter in slot 1");
        pc_fdc_card_connect_drive(fdc, 0, drive);
        boot();
        CHECK(pc_status(pc) == PC_HALTED && on_screen("No boot device available."), "empty drive: no boot device");
        CHECK((m[0x410] & 0xC1) == 0x01, "the equipment word lists one diskette drive");

        fdd_insert(drive, disk);
        boot();
        CHECK(on_screen("The diskette in drive A: is not bootable."), "an unformatted diskette has no boot signature");

        fat12_format(disk, "PC86");
        boot();
        CHECK(on_screen("Non-system disk") == 1 && on_screen("Press any key to restart") == 1,
              "a formatted diskette boots and its boot sector says it is not a system diskette");
        type(0x39);
        run_frames(3);
        CHECK(on_screen("Non-system disk") == 2, "a key: the boot sector calls INT 19h and boots again");

        if (asm86_assemble(BOOT_SECTOR, &a) != 0 || a.error_count) {
            printf("boot sector: line %d: %s\n", a.errors[0].line, a.errors[0].message);
            exit(1);
        }
        CHECK(fat12_install_boot(disk, a.code, a.size) == 0, "boot sector installed on the diskette");
        img = disk_data(disk);
        {
            int i;
            for (i = 0; i < 512; i++) img[(1 * 36 + 2) * 512 + i] = (uint8_t)(i ^ 0x5A);   /* cylinder 1, sector 3 */
        }
        boot();
        run_frames(60);
        CHECK(m[boot_sym(&a, "done")] == 1, "the boot sector ran to the end");
        CHECK(m[boot_sym(&a, "drive")] == 0 && m[boot_sym(&a, "bios_status")] == 0 && !memcmp(m + 0x8000, img + 512, 512),
              "booted with DL = 0; INT 13h read sector 2");
        CHECK(m[boot_sym(&a, "seek_st0")] == 0x20 && m[boot_sym(&a, "seek_pcn")] == 1,
              "SEEK on the controller, IRQ 6 through the BIOS's INT 0Eh, SENSE INTERRUPT STATUS");
        CHECK(m[boot_sym(&a, "result")] == 0 && m[boot_sym(&a, "result") + 5] == 4,
              "READ DATA ends normally at the DMA terminal count");
        CHECK(!memcmp(m + 0x9000, img + (1 * 36 + 2) * 512, 512), "and the 8237 put the sector at 0000:9000");
        CHECK(fdd_motor(drive) && (m[0x43F] & 1), "the motor is on");
        run_frames(150);
        CHECK(!fdd_motor(drive) && !(m[0x43F] & 1), "two seconds later the BIOS timer interrupt turns it off");
        asm86_free(&a);

        /* hot-swapping the diskette */
        fdd_eject(drive);
        load("org 100h\n"
             "    mov ah, 16h\n"
             "    xor dl, dl\n"
             "    int 13h\n"
             "    mov [changed], ah\n"
             "    mov ax, 0201h\n"
             "    mov bx, buf\n"
             "    mov cx, 0001h\n"
             "    xor dx, dx\n"
             "    int 13h\n"
             "    mov [empty], ah\n"
             "    int 20h\n"
             "changed db 0, 0\n"
             "empty db 0, 0\n"
             "buf times 512 db 0\n");
        run_frames(2);
        CHECK((var("changed") & 0xFF) == 0x06 && (var("empty") & 0xFF) == 0x80,
              "diskette out: INT 13h AH=16h says changed, a read times out");
        fdd_insert(drive, disk);
        disk_clear_modified(disk);
        load("org 100h\n"
             "    mov ax, 0201h\n"
             "    mov bx, buf\n"
             "    mov cx, 0001h\n"
             "    xor dx, dx\n"
             "    int 13h\n"
             "    mov [first], ah\n"
             "    mov ax, 0201h\n"
             "    mov cx, 0001h\n"
             "    int 13h\n"
             "    mov [second], ah\n"
             "    mov word [buf], 1234h\n"
             "    mov ax, 0301h\n"
             "    mov cx, 0201h\n"                  /* cylinder 2, sector 1 */
             "    mov dh, 1\n"
             "    int 13h\n"
             "    mov [third], ah\n"
             "    int 20h\n"
             "first db 0, 0\n"
             "second db 0, 0\n"
             "third db 0, 0\n"
             "buf times 512 db 0\n");
        run_frames(2);
        CHECK((var("first") & 0xFF) == 0x06 && (var("second") & 0xFF) == 0x00, "back in: media changed once, then it reads");
        CHECK((var("third") & 0xFF) == 0x00 && img[(2 * 36 + 18) * 512] == 0x34 && img[(2 * 36 + 18) * 512 + 1] == 0x12 &&
              disk_modified(disk), "INT 13h writes to the diskette");

        /* state with the adapter in a slot */
        len = pc_state_size(pc);
        state = (uint8_t *)malloc(len);
        CHECK(pc_save_state(pc, state, len) == len && pc_load_state(pc, state, len) == 0, "state with the diskette adapter");
        free(state);

        /* a DMA mode the 8237 emulation does not have */
        load("org 100h\n"
             "    mov al, 0C6h\n"                   /* cascade mode on channel 2 */
             "    out 0Bh, al\n"
             "    mov al, 02h\n"
             "    out 0Ah, al\n"
             "    mov dx, 3F5h\n"
             "    mov si, cmd\n"
             "    mov cx, 9\n"
             "next:\n"
             "    lodsb\n"
             "    out dx, al\n"
             "    loop next\n"
             "idle:\n"
             "    hlt\n"
             "    jmp idle\n"
             "cmd db 0E6h, 4, 2, 1, 1, 2, 18, 1Bh, 0FFh\n");   /* where the BIOS left the head */
        run_frames(60);
        CHECK(pc_status(pc) == PC_ERROR && strstr(pc_error(pc), "DMA") != NULL, "an unsupported DMA mode is reported");
        printf("     (%s)\n", pc_error(pc));

        pc_power_off(pc);
        CHECK(pc_remove_card(pc, 1) == pc_fdc_card_slot(fdc) && !fdd_motor(drive), "adapter removed; the drive stops");
        fdd_eject(drive);
        pc_fdc_card_destroy(fdc);
        fdd_destroy(drive);
        disk_destroy(disk);
    }

    /* ---------- a machine without a video card ---------- */
    pc_power_off(pc);
    pc_remove_card(pc, 0);
    pc_power_on(pc);
    run_frames(3);
    load("org 100h\n"
         "    mov ax, 0E41h\n"
         "    int 10h\n"
         "    int 20h\n");
    run_frames(2);
    CHECK(pc_status(pc) == PC_STOPPED, "without a video card INT 10h does nothing and the program goes on");
    pc_power_off(pc);
    CHECK(!kbd_powered(kbd) && pc_status(pc) == PC_OFF, "power off: the keyboard goes dark");

    pc_destroy(pc);
    pc_vga_card_destroy(vga);
    monitor_destroy(mono); monitor_destroy(color); monitor_destroy(cga);
    kbd_destroy(kbd);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "All passed", fails);
    return fails ? 1 : 0;
}
