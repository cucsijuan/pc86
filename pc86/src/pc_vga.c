/*
 * pc86 - VGA card for the expansion slots. See include/pc_vga.h.
 */
#include "pc_vga.h"

#include <stdlib.h>
#include <string.h>

struct pc_vga_card {
    pc_card_t card;
    vga_t *vga;
    monitor_t *monitor;
    int on;
    uint8_t rgb[VGA_OUT_WIDTH * VGA_OUT_HEIGHT * 3];
};

static void update_sense(pc_vga_card_t *c)
{
    if (c->vga) vga_set_monitor_sense(c->vga, c->monitor ? monitor_sense(c->monitor) : VGA_SENSE_NONE);
}

static void c_attach(void *user, uint8_t *memory)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    if (c->vga) vga_set_memory(c->vga, memory);
    else c->vga = vga_create(memory);
    update_sense(c);
}

static void c_power(void *user, int on)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    c->on = on && c->vga;
    if (c->on) vga_reset(c->vga);   /* the option ROM sets mode 03h */
}

static int c_port_in(void *user, uint16_t port, uint8_t *value)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    return c->on ? vga_port_in(c->vga, port, value) : PC_PORT_NOT_MINE;
}

static int c_port_out(void *user, uint16_t port, uint8_t value)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    return c->on ? vga_port_out(c->vga, port, value) : PC_PORT_NOT_MINE;
}

static void c_advance(void *user, uint32_t microseconds)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    if (c->on) vga_advance(c->vga, microseconds);
}

static int c_bios(void *user, uint8_t number, vm86_regs_t *r)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    vga_bios_regs_t b;
    if (!c->on || number != 0x10) return PC_BIOS_UNSUPPORTED;
    b.ax = r->ax; b.bx = r->bx; b.cx = r->cx; b.dx = r->dx; b.es = r->es; b.bp = r->bp;
    if (!vga_int10(c->vga, &b)) return PC_BIOS_UNSUPPORTED;
    r->ax = b.ax; r->bx = b.bx; r->cx = b.cx; r->dx = b.dx;
    return PC_BIOS_DONE;
}

static size_t c_state_size(void *user) { (void)user; return vga_state_size(); }

static size_t c_save_state(void *user, void *buf, size_t len)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    return c->vga ? vga_save_state(c->vga, buf, len) : 0;
}

static int c_load_state(void *user, const void *buf, size_t len)
{
    pc_vga_card_t *c = (pc_vga_card_t *)user;
    if (!c->vga || vga_load_state(c->vga, buf, len) != 0) return -1;
    update_sense(c);              /* the monitor plugged in now is what the pins say */
    return 0;
}

pc_vga_card_t *pc_vga_card_create(void)
{
    pc_vga_card_t *c = (pc_vga_card_t *)calloc(1, sizeof(pc_vga_card_t));
    if (!c) return NULL;
    c->card.name = "VGA";
    c->card.user = c;
    c->card.attach = c_attach;
    c->card.power = c_power;
    c->card.port_in = c_port_in;
    c->card.port_out = c_port_out;
    c->card.advance = c_advance;
    c->card.rom_ints[0] = 0x10;
    c->card.rom_int_count = 1;
    c->card.bios = c_bios;
    c->card.state_size = c_state_size;
    c->card.save_state = c_save_state;
    c->card.load_state = c_load_state;
    return c;
}

void pc_vga_card_destroy(pc_vga_card_t *c)
{
    if (!c) return;
    if (c->vga) vga_destroy(c->vga);
    free(c);
}

pc_card_t *pc_vga_card_slot(pc_vga_card_t *c) { return &c->card; }
vga_t *pc_vga_card_vga(pc_vga_card_t *c) { return c->vga; }
monitor_t *pc_vga_card_monitor(pc_vga_card_t *c) { return c->monitor; }

int pc_vga_card_connect_monitor(pc_vga_card_t *c, monitor_t *monitor)
{
    if (monitor && monitor_model(monitor)->connector != MONITOR_CONN_VGA) return -1;
    c->monitor = monitor;
    update_sense(c);
    return 0;
}

int pc_vga_card_display(pc_vga_card_t *c, float dt_seconds, uint8_t *out)
{
    vga_signal_t sig;
    if (!c->monitor) return -1;
    if (!c->on) return monitor_display(c->monitor, NULL, NULL, dt_seconds, out);
    vga_render_rgb(c->vga, c->rgb);
    vga_signal(c->vga, &sig);
    return monitor_display(c->monitor, &sig, c->rgb, dt_seconds, out);
}
