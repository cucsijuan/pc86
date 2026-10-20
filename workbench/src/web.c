/*
 * workbench - the browser side of a pc86 machine, compiled to WebAssembly.
 *
 * The page assembles the program in the editor, runs it on a complete
 * machine (motherboard, VGA card in a slot, monitor, keyboard) and lets the
 * user switch the power, change the card, and plug peripherals in and out.
 * Everything is called from index.html through the web_* exports.
 *
 * A tiny DOS is provided for the program, as in pcrun: INT 20h and
 * INT 21h AH=02h, 09h, 4Ch.
 */
#include "asm86.h"
#include "pc.h"
#include "pc_vga.h"

#include <stdio.h>
#include <string.h>

#define WEB(name) __attribute__((export_name(#name))) name

#define SRC_CAP (256 * 1024)

/* What the page shows next to the run light. */
enum {
    WEB_RUNNING = 0,
    WEB_HALTED,
    WEB_FINISHED,
    WEB_ERROR,
    WEB_IDLE,
    WEB_OFF
};

static pc_t *pc;
static pc_vga_card_t *vga;
static monitor_t *monitors[MONITOR_MODEL_COUNT];
static kbd_t *keyboards[KBD_MODEL_COUNT];
static int monitor_id = -1, keyboard_id = -1, card_in;

static char src[SRC_CAP + 1];
static char scratch[64];
static char message[300];
static asm86_result_t prog;
static int have_prog, loaded, exit_code, error_line;
static uint8_t pixels[VGA_OUT_WIDTH * VGA_OUT_HEIGHT * 3];

/* ---------- DOS ---------- */

static void teletype(uint8_t ch)
{
    vga_bios_regs_t b;
    vga_t *v = card_in ? pc_vga_card_vga(vga) : NULL;
    if (!v) return;
    memset(&b, 0, sizeof(b));
    b.ax = (uint16_t)(0x0E00 | ch);
    b.bx = 0x0007;
    vga_int10(v, &b);
}

static int dos_int(void *user, pc_t *m, uint8_t n)
{
    vm86_regs_t *r = vm86_regs(pc_cpu(m));
    (void)user;
    if (n == 0x20) { exit_code = 0; pc_stop(m); return 1; }
    if (n != 0x21) return 0;
    switch (r->ax >> 8) {
    case 0x02: teletype((uint8_t)r->dx); return 1;
    case 0x09: {
        uint8_t *mem = pc_memory(m);
        uint32_t a = ((uint32_t)r->ds << 4) + r->dx;
        int guard = 0;
        while (mem[a & 0xFFFFF] != '$' && guard++ < 65536) teletype(mem[a++ & 0xFFFFF]);
        return 1;
    }
    case 0x4C: exit_code = r->ax & 0xFF; pc_stop(m); return 1;
    default: return 0;
    }
}

/* ---------- machine ---------- */

void WEB(web_init)(void)
{
    int i;
    pc = pc_create();
    vga = pc_vga_card_create();
    for (i = 0; i < MONITOR_MODEL_COUNT; i++) monitors[i] = monitor_create(monitor_builtin_model(i));
    for (i = 0; i < KBD_MODEL_COUNT; i++) keyboards[i] = kbd_create(kbd_builtin_model(i));
    pc_set_int_hook(pc, dos_int, NULL);
    card_in = pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == 0;
    monitor_id = MONITOR_MODEL_VGA_MONO;
    pc_vga_card_connect_monitor(vga, monitors[monitor_id]);
    keyboard_id = KBD_MODEL_ENHANCED101;
    pc_connect_keyboard(pc, keyboards[keyboard_id]);
    pc_power_on(pc);
}

/* Power switch. Turning it on boots the machine; without a disk the BIOS
 * says so and waits, so the program is loaded again with web_start(). */
void WEB(web_power)(int on)
{
    loaded = 0;
    if (on) pc_power_on(pc);
    else pc_power_off(pc);
}
int WEB(web_powered)(void) { return pc_status(pc) != PC_OFF; }

/* Slot 1 (index 0): the VGA card. Only with the power off. */
int WEB(web_card)(int insert)
{
    if (pc_status(pc) != PC_OFF) return -1;
    if (insert && !card_in) card_in = pc_insert_card(pc, 0, pc_vga_card_slot(vga)) == 0;
    else if (!insert && card_in) card_in = pc_remove_card(pc, 0) == NULL;
    return card_in;
}
int WEB(web_card_in)(void) { return card_in; }

/* Monitor connector of the VGA card: a MONITOR_MODEL_* or -1 to unplug.
 * Returns -1 when the monitor's plug does not fit the card. */
int WEB(web_set_monitor)(int id)
{
    if (id < 0 || id >= MONITOR_MODEL_COUNT) {
        pc_vga_card_connect_monitor(vga, NULL);
        monitor_id = -1;
        return 0;
    }
    if (pc_vga_card_connect_monitor(vga, monitors[id]) != 0) return -1;
    monitor_id = id;
    return 0;
}
int WEB(web_monitor)(void) { return monitor_id; }
void WEB(web_monitor_power)(int on) { if (monitor_id >= 0) monitor_set_power(monitors[monitor_id], on); }
int WEB(web_monitor_powered)(void) { return monitor_id >= 0 && monitor_power(monitors[monitor_id]); }
int WEB(web_monitor_color)(void) { return monitor_id >= 0 && monitor_model(monitors[monitor_id])->color; }
const char *WEB(web_monitor_name)(int id)
{
    const monitor_model_t *m = monitor_builtin_model(id);
    return m ? m->name : "";
}
int WEB(web_monitor_count)(void) { return MONITOR_MODEL_COUNT; }

/* Keyboard connector of the board: a KBD_MODEL_* or -1 to unplug. */
void WEB(web_set_keyboard)(int id)
{
    if (keyboard_id >= 0) kbd_release_all(keyboards[keyboard_id]);
    keyboard_id = id >= 0 && id < KBD_MODEL_COUNT ? id : -1;
    pc_connect_keyboard(pc, keyboard_id >= 0 ? keyboards[keyboard_id] : NULL);
}
int WEB(web_keyboard)(void) { return keyboard_id; }
const char *WEB(web_keyboard_name)(int id)
{
    const kbd_model_t *m = kbd_builtin_model(id);
    return m ? m->name : "";
}
int WEB(web_keyboard_count)(void) { return KBD_MODEL_COUNT; }

/* ---------- program ---------- */

char *WEB(web_src)(void) { return src; }
int WEB(web_src_cap)(void) { return SRC_CAP; }

/* Assembles the len bytes at web_src(). Returns the number of errors. */
int WEB(web_assemble)(int len)
{
    if (len < 0 || len > SRC_CAP) len = 0;
    src[len] = 0;
    if (have_prog) asm86_free(&prog);
    have_prog = 1;
    asm86_assemble(src, &prog);
    return prog.error_count;
}
int WEB(web_err_line)(int i) { return i >= 0 && i < prog.error_count ? prog.errors[i].line : 0; }
const char *WEB(web_err_msg)(int i) { return i >= 0 && i < prog.error_count ? prog.errors[i].message : ""; }
int WEB(web_code_size)(void) { return have_prog ? (int)prog.size : 0; }

/* Restarts the machine (power cycle, or power on) and loads the assembled
 * program as a .COM. 0, or -1 if there is no program to load. */
int WEB(web_start)(void)
{
    if (!have_prog || prog.error_count) return -1;
    if (pc_status(pc) == PC_OFF) pc_power_on(pc);
    else pc_reset(pc);
    exit_code = 0;
    error_line = 0;
    loaded = pc_load_com(pc, prog.code, prog.size) == 0;
    return loaded ? 0 : -1;
}

void WEB(web_stop)(void) { pc_stop(pc); }

/* Source line of the program at offset ip of its segment, or 0. */
static int line_at(uint16_t ip)
{
    int i;
    for (i = 0; i < prog.line_count; i++)
        if (prog.lines[i].size && ip >= prog.lines[i].address &&
            ip < prog.lines[i].address + prog.lines[i].size)
            return prog.lines[i].line;
    return 0;
}

static int state(void)
{
    switch (pc_status(pc)) {
    case PC_OFF: return WEB_OFF;
    case PC_RUNNING: return loaded ? WEB_RUNNING : WEB_IDLE;
    case PC_HALTED: return loaded ? WEB_HALTED : WEB_IDLE;
    case PC_STOPPED: return WEB_FINISHED;
    case PC_ERROR: return WEB_ERROR;
    }
    return WEB_IDLE;
}

/* Runs dt seconds of emulated time at ips instructions per second. */
int WEB(web_frame)(double dt, int ips)
{
    int before = pc_status(pc);
    if (ips > 0) pc_set_speed(pc, (uint32_t)ips);
    pc_run(pc, dt);
    if (pc_status(pc) == PC_ERROR && before != PC_ERROR && have_prog) {
        /* An unsupported instruction is reported at its own address; the
         * rest (ports, BIOS calls) once the instruction has executed. */
        vm86_regs_t *r = vm86_regs(pc_cpu(pc));
        if (r->cs == PC_COM_SEGMENT)
            error_line = strncmp(pc_error(pc), "unsupported instruction", 23) == 0
                             ? line_at(r->ip) : line_at((uint16_t)(r->ip - 1));
    }
    return state();
}
int WEB(web_state)(void) { return state(); }
int WEB(web_error_line)(void) { return error_line; }

const char *WEB(web_message)(void)
{
    switch (state()) {
    case WEB_OFF: return "Power is off.";
    case WEB_RUNNING: return "Running";
    case WEB_HALTED: return "Waiting for an interrupt (HLT)";
    case WEB_FINISHED:
        if (exit_code) snprintf(message, sizeof(message), "Program finished with exit code %d.", exit_code);
        else snprintf(message, sizeof(message), "Program finished.");
        return message;
    case WEB_ERROR:
        snprintf(message, sizeof(message), "%s", pc_error(pc));
        if (message[0]) message[0] = (char)(message[0] - (message[0] >= 'a' && message[0] <= 'z' ? 32 : 0));
        return message;
    default: return card_in ? "No boot device available." : "No video card: nothing to show.";
    }
}

double WEB(web_seconds)(void) { return (double)pc_time_us(pc) / 1e6; }
int WEB(web_mode)(void) { return card_in && pc_vga_card_vga(vga) ? vga_mode(pc_vga_card_vga(vga)) : 0; }
double WEB(web_speaker_hz)(void) { return pc_speaker_hz(pc); }

/* ---------- monitor ---------- */

/* What the monitor shows: MONITOR_* (pixels in web_pixels(), one byte per
 * pixel for mono, three for color), or -1 with no monitor connected. A
 * monitor plugged into nothing, or into a card in an empty machine, gets no
 * signal. */
int WEB(web_display)(float dt)
{
    monitor_t *m;
    if (monitor_id < 0) return -1;
    m = monitors[monitor_id];
    if (card_in) return pc_vga_card_display(vga, dt, pixels);
    return monitor_display(m, NULL, NULL, dt, pixels);
}
uint8_t *WEB(web_pixels)(void) { return pixels; }

/* ---------- keyboard ---------- */

char *WEB(web_scratch)(void) { return scratch; }

/* Key with the KeyboardEvent.code at web_scratch(). 1 if it is a PC key. */
int WEB(web_key)(int down)
{
    uint16_t key;
    scratch[sizeof(scratch) - 1] = 0;
    key = kbd_key_from_code(scratch);
    if (!key) return 0;
    if (keyboard_id >= 0) kbd_key(keyboards[keyboard_id], key, down);
    return 1;
}
void WEB(web_release_keys)(void) { if (keyboard_id >= 0) kbd_release_all(keyboards[keyboard_id]); }
int WEB(web_leds)(void) { return keyboard_id >= 0 ? kbd_leds(keyboards[keyboard_id]) : 0; }
int WEB(web_has_key)(void)
{
    uint16_t key = kbd_key_from_code(scratch);
    return keyboard_id >= 0 && key && kbd_has_key(kbd_model(keyboards[keyboard_id]), key);
}
