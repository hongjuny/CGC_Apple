// Mouse card in slot 2, compatible with the AppleMouse II firmware interface.
//
// The card has its own small firmware (no ROM dump needed). Programs find it
// by its ID bytes ($Cn05=$38, $Cn07=$18, $Cn0B=$01, $Cn0C=$20, $CnFB=$D6) and
// call the routines whose offsets are at $Cn12-$Cn19, passing data through the
// slot's screen holes. Each routine writes A to one I/O register; the
// emulator does the work and returns the carry through another.
//
// Screen holes for slot n: $0478+n / $0578+n X low/high, $04F8+n / $05F8+n
// Y low/high, $0778+n status, $07F8+n mode. ClampMouse reads its bounds from
// $0478/$04F8 (min/max low) and $0578/$05F8 (min/max high).
#include <string.h>
#include "a2internal.h"

enum { MOUSE_SLOT = 2 };

enum {                              // routines, in $Cn12-$Cn19 order
    FN_SET, FN_SERVE, FN_READ, FN_CLEAR, FN_POS, FN_CLAMP, FN_HOME, FN_INIT, FN_COUNT,
};

enum {                              // mode bits (SetMouse)
    MODE_ON = 0x01, MODE_INT_MOVE = 0x02, MODE_INT_BUTTON = 0x04, MODE_INT_VBL = 0x08,
};

enum {                              // status bits ($0778+n)
    ST_BUTTON = 0x80, ST_LAST_BUTTON = 0x40, ST_MOVED = 0x20,
    ST_INT_VBL = 0x08, ST_INT_BUTTON = 0x04, ST_INT_MOVE = 0x02,
};

#define ENTRIES 0x40                // routine stubs, 8 bytes each
#define RESULT_REG 0x0F             // $C0AF: carry of the last routine

static void build_rom(uint8_t *rom)
{
    memset(rom, 0, 256);
    rom[0x00] = 0x60;               // PR#2 / IN#2: RTS, nothing to print or read
    rom[0x05] = 0x38;               // ID bytes
    rom[0x07] = 0x18;
    rom[0x0B] = 0x01;               // Pascal 1.1 firmware protocol
    rom[0x0C] = 0x20;               // device: X = 2 (mouse), Y = 0
    for (int i = 0x0D; i <= 0x10; i++)
        rom[i] = 0x30;              // Pascal entries: report an error
    rom[0x30] = 0xA2; rom[0x31] = 0x03;   // LDX #3 (not supported)
    rom[0x32] = 0x38;                     // SEC
    rom[0x33] = 0x60;                     // RTS
    for (int fn = 0; fn < FN_COUNT; fn++) {
        uint8_t *p = rom + ENTRIES + fn * 8;
        rom[0x12 + fn] = (uint8_t)(ENTRIES + fn * 8);
        p[0] = 0x8D; p[1] = (uint8_t)(0xA0 + fn); p[2] = 0xC0;   // STA $C0A0+fn (do it)
        p[3] = 0xAD; p[4] = 0xA0 + RESULT_REG; p[5] = 0xC0;      // LDA $C0AF (0 or 1)
        p[6] = 0x4A;                                             // LSR A: result -> carry
        p[7] = 0x60;                                             // RTS
    }
    rom[0xFB] = 0xD6;               // AppleMouse II ID
}

// ---- screen holes -------------------------------------------------------------

static uint16_t hole(uint16_t base) { return (uint16_t)(base + MOUSE_SLOT); }

static void write_position(apple2 *a2)
{
    mousecard *m = &a2->mouse;
    bus_write(a2, hole(0x478), (uint8_t)m->x);
    bus_write(a2, hole(0x578), (uint8_t)(m->x >> 8));
    bus_write(a2, hole(0x4F8), (uint8_t)m->y);
    bus_write(a2, hole(0x5F8), (uint8_t)(m->y >> 8));
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void clamp_position(mousecard *m)
{
    m->x = clamp(m->x, m->min_x, m->max_x);
    m->y = clamp(m->y, m->min_y, m->max_y);
}

static void init_state(mousecard *m)
{
    m->min_x = m->min_y = 0;
    m->max_x = m->max_y = 1023;
    m->x = m->y = 0;
    m->mode = 0;
    m->moved = m->last_button = false;
    m->moved_since_vbl = m->button_changed = false;
    m->pending = 0;
}

// Moves the Apple II position to the host pointer, mapped over the clamp window.
static void follow_host(mousecard *m)
{
    if (!m->host_valid)
        return;
    int x = m->min_x + (int)(m->host_x * (float)(m->max_x - m->min_x) + 0.5f);
    int y = m->min_y + (int)(m->host_y * (float)(m->max_y - m->min_y) + 0.5f);
    x = clamp(x, m->min_x, m->max_x);
    y = clamp(y, m->min_y, m->max_y);
    if (x != m->x || y != m->y) {
        m->x = x;
        m->y = y;
        m->moved = m->moved_since_vbl = true;
    }
}

// ---- routines -----------------------------------------------------------------

static uint8_t run(apple2 *a2, int fn, uint8_t a)
{
    mousecard *m = &a2->mouse;
    switch (fn) {
    case FN_SET:
        if (a > 0x0F)
            return 1;
        m->mode = a;
        if (!(a & (MODE_INT_MOVE | MODE_INT_BUTTON | MODE_INT_VBL)))
            m->pending = 0;
        bus_write(a2, hole(0x7F8), a);
        return 0;
    case FN_SERVE: {
        // Reports which interrupt sources fired; carry clear if the mouse interrupted.
        uint8_t st = (uint8_t)((bus_read(a2, hole(0x778)) & ~(ST_INT_VBL | ST_INT_BUTTON | ST_INT_MOVE)) | m->pending);
        bus_write(a2, hole(0x778), st);
        bool ours = m->pending != 0;
        m->pending = 0;
        return ours ? 0 : 1;
    }
    case FN_READ: {
        follow_host(m);
        write_position(a2);
        uint8_t st = (uint8_t)((m->button ? ST_BUTTON : 0) | (m->last_button ? ST_LAST_BUTTON : 0) |
                               (m->moved ? ST_MOVED : 0));
        bus_write(a2, hole(0x778), st);
        m->last_button = m->button;
        m->moved = false;
        return 0;
    }
    case FN_CLEAR:
        m->x = m->y = 0;
        m->host_valid = false;      // until the pointer moves again
        write_position(a2);
        return 0;
    case FN_POS:
        m->x = bus_read(a2, hole(0x478)) | bus_read(a2, hole(0x578)) << 8;
        m->y = bus_read(a2, hole(0x4F8)) | bus_read(a2, hole(0x5F8)) << 8;
        clamp_position(m);
        m->host_valid = false;
        return 0;
    case FN_CLAMP: {
        int lo = (int16_t)(bus_read(a2, 0x478) | bus_read(a2, 0x578) << 8);   // signed bounds
        int hi = (int16_t)(bus_read(a2, 0x4F8) | bus_read(a2, 0x5F8) << 8);
        if (a == 0) {
            m->min_x = lo;
            m->max_x = hi;
        } else {
            m->min_y = lo;
            m->max_y = hi;
        }
        clamp_position(m);
        return 0;
    }
    case FN_HOME:
        m->x = m->min_x;
        m->y = m->min_y;
        m->host_valid = false;
        write_position(a2);
        return 0;
    case FN_INIT:
        init_state(m);
        m->host_valid = false;
        write_position(a2);
        bus_write(a2, hole(0x778), 0);
        bus_write(a2, hole(0x7F8), 0);
        return 0;
    default:
        return 1;
    }
}

uint8_t mouse_rom_read(const apple2 *a2, uint16_t addr)
{
    return a2->mouse.rom[addr & 0xFF];
}

uint8_t mouse_io(apple2 *a2, uint8_t reg, bool write, uint8_t value)
{
    mousecard *m = &a2->mouse;
    if (reg == RESULT_REG)
        return write ? 0 : m->result;
    if (write && reg < FN_COUNT)
        m->result = run(a2, reg, value);
    return 0;
}

// Called at each vertical blank: the card samples the mouse and raises its
// interrupts then, as the real one does.
void mouse_vbl(apple2 *a2)
{
    mousecard *m = &a2->mouse;
    if (!m->enabled || !(m->mode & MODE_ON))
        return;
    follow_host(m);
    if (m->mode & MODE_INT_VBL)
        m->pending |= ST_INT_VBL;
    if ((m->mode & MODE_INT_MOVE) && m->moved_since_vbl)
        m->pending |= ST_INT_MOVE;
    if ((m->mode & MODE_INT_BUTTON) && m->button_changed)
        m->pending |= ST_INT_BUTTON;
    m->moved_since_vbl = m->button_changed = false;
}

bool mouse_irq(const apple2 *a2)
{
    return a2->mouse.enabled && a2->mouse.pending != 0;
}

// RESET clears the card's mode and interrupts; the host pointer is kept.
void mouse_reset(apple2 *a2)
{
    mousecard *m = &a2->mouse;
    init_state(m);
    m->result = 0;
}

// ---- public API -------------------------------------------------------------

bool a2_set_mouse(apple2 *a2, bool enabled)
{
    mousecard *m = &a2->mouse;
    memset(m, 0, sizeof *m);
    if (enabled) {
        build_rom(m->rom);
        init_state(m);
        m->enabled = true;
    }
    a2->cpu.irq_line = false;
    return true;
}

bool a2_mouse_enabled(const apple2 *a2) { return a2->mouse.enabled; }

void a2_mouse_move(apple2 *a2, float x, float y)
{
    mousecard *m = &a2->mouse;
    m->host_x = x < 0 ? 0 : x > 1 ? 1 : x;
    m->host_y = y < 0 ? 0 : y > 1 ? 1 : y;
    m->host_valid = true;
}

void a2_mouse_button(apple2 *a2, bool down)
{
    mousecard *m = &a2->mouse;
    if (m->button != down)
        m->button_changed = true;
    m->button = down;
}
