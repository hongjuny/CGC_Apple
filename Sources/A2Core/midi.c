// Passport MIDI Interface (and compatible 6850 MIDI cards): a 6840 PTM
// (three programmable timers) at $C0n0-$C0n7 and a 6850 ACIA at $C0n8-$C0n9,
// with MIDI output only. Card wiring follows MAME's
// src/devices/bus/a2bus/a2midi.cpp (BSD-3-Clause, R. Belmont); the PTM
// register behaviour follows MAME's machine/6840ptm.cpp.
#include <string.h>
#include "a2internal.h"

#define MIDI_BYTE_CYCLES 327   // 10 bits at 31250 baud = 320 us

// ---- 6840 PTM -----------------------------------------------------------------

enum {
    CR_RESET = 0x01,        // CR1 only: hold all timers in reset
    CR_SELECT = 0x01,       // CR2 only: register 0 writes CR1 (else CR3)
    CR_DUAL8 = 0x04,        // dual 8-bit counting
    CR_NO_INIT = 0x10,      // latch writes do not reload the counter
    CR_ONESHOT = 0x20,      // modes 4 and 6
    CR_IRQ_EN = 0x40,
};

static uint32_t ptm_period(const ptm6840 *p, int t)
{
    uint16_t latch = p->latch[t];
    if (p->cr[t] & CR_DUAL8)
        return (uint32_t)((latch & 0xFF) + 1) * (uint32_t)((latch >> 8) + 1);
    return (uint32_t)latch + 1;
}

static void ptm_reload(ptm6840 *p, int t)
{
    p->remaining[t] = (int32_t)ptm_period(p, t);
    p->fired[t] = false;
}

static void ptm_reset(ptm6840 *p)
{
    memset(p, 0, sizeof *p);
    p->cr[0] = CR_RESET;
    for (int t = 0; t < 3; t++) {
        p->latch[t] = 0xFFFF;
        ptm_reload(p, t);
    }
}

static void ptm_tick(ptm6840 *p, int cycles)
{
    if (p->cr[0] & CR_RESET)
        return;
    for (int t = 0; t < 3; t++) {
        int n = cycles;
        if (t == 2 && (p->cr[2] & 0x01)) {       // timer 3 prescaler: divide by 8
            p->prescale += cycles;
            n = p->prescale / 8;
            p->prescale %= 8;
        }
        p->remaining[t] -= n;
        while (p->remaining[t] <= 0) {
            bool one_shot = p->cr[t] & CR_ONESHOT;
            if (!one_shot || !p->fired[t]) {
                p->status |= (uint8_t)(1 << t);
                p->status_read &= (uint8_t)~(1 << t);
            }
            p->fired[t] = true;
            p->remaining[t] += (int32_t)ptm_period(p, t);
        }
    }
}

static bool ptm_irq(const ptm6840 *p)
{
    for (int t = 0; t < 3; t++)
        if ((p->status & (1 << t)) && (p->cr[t] & CR_IRQ_EN))
            return true;
    return false;
}

static uint16_t ptm_counter(const ptm6840 *p, int t)
{
    return (uint16_t)(p->remaining[t] - 1);
}

static uint8_t ptm_read(ptm6840 *p, int reg)
{
    switch (reg) {
    case 1:
        p->status_read |= p->status & 0x07;
        return (uint8_t)(p->status | (ptm_irq(p) ? 0x80 : 0));
    case 2: case 4: case 6: {
        int t = (reg - 2) / 2;
        uint16_t c = ptm_counter(p, t);
        // Reading the counter after the status register clears that timer's flag.
        if (p->status_read & (1 << t))
            p->status &= (uint8_t)~(1 << t);
        p->lsb_buffer = (uint8_t)c;
        return (uint8_t)(c >> 8);
    }
    case 3: case 5: case 7:
        return p->lsb_buffer;
    default:
        return 0;
    }
}

static void ptm_write(ptm6840 *p, int reg, uint8_t value)
{
    switch (reg) {
    case 0: case 1: {
        int t = reg == 1 ? 1 : (p->cr[1] & CR_SELECT) ? 0 : 2;
        bool was_reset = p->cr[0] & CR_RESET;
        p->cr[t] = value;
        if (t == 0 && was_reset != (bool)(value & CR_RESET)) {
            p->status = p->status_read = 0;
            for (int i = 0; i < 3; i++)
                ptm_reload(p, i);
        }
        break;
    }
    case 2: case 4: case 6:
        p->msb_buffer = value;
        break;
    case 3: case 5: case 7: {
        int t = (reg - 3) / 2;
        p->latch[t] = (uint16_t)((p->msb_buffer << 8) | value);
        p->status &= (uint8_t)~(1 << t);
        if (!(p->cr[t] & CR_NO_INIT) || (p->cr[0] & CR_RESET))
            ptm_reload(p, t);
        break;
    }
    }
}

// ---- 6850 ACIA (transmit side) ------------------------------------------------

enum { SR_RDRF = 0x01, SR_TDRE = 0x02, SR_IRQ = 0x80 };

static void midi_emit(passport_midi *m, uint8_t byte)
{
    if (m->out_count < sizeof m->out)
        m->out[m->out_count++] = byte;
}

// Moves a waiting byte into the transmit shifter once the previous one is out.
static void acia_update(passport_midi *m, uint64_t cycle)
{
    if (m->tdr_full && cycle >= m->shifter_free) {
        // It started when both the shifter was free and the byte had been written.
        uint64_t start = m->shifter_free > m->tdr_cycle ? m->shifter_free : m->tdr_cycle;
        m->shifter_free = start + MIDI_BYTE_CYCLES;
        m->tdr_full = false;
        midi_emit(m, m->tdr);
    }
}

static bool acia_irq(const passport_midi *m)
{
    bool tx_irq = (m->acia_control & 0x60) == 0x20 && !m->tdr_full;
    return !m->acia_reset && tx_irq;
}

static uint8_t acia_read(passport_midi *m, int reg, uint64_t cycle)
{
    acia_update(m, cycle);
    if (reg == 1)
        return 0;                       // receive data: no MIDI input connected
    if (m->acia_reset)
        return 0;
    // CTS and DCD are tied active (low) on the card.
    return (uint8_t)((m->tdr_full ? 0 : SR_TDRE) | (acia_irq(m) ? SR_IRQ : 0));
}

static void acia_write(passport_midi *m, int reg, uint8_t value, uint64_t cycle)
{
    acia_update(m, cycle);
    if (reg == 0) {
        m->acia_control = value;
        m->acia_reset = (value & 0x03) == 0x03;   // master reset
        if (m->acia_reset)
            m->tdr_full = false;
        return;
    }
    if (m->acia_reset)
        return;
    m->tdr = value;
    m->tdr_full = true;
    m->tdr_cycle = cycle;
    acia_update(m, cycle);
}

// ---- card ---------------------------------------------------------------------

void midi_reset(passport_midi *m)
{
    // The ACIA has no reset input; the PTM is reset with the bus.
    ptm_reset(&m->ptm);
}

void midi_power_on(passport_midi *m)
{
    memset(m, 0, sizeof *m);
    m->acia_reset = true;
    ptm_reset(&m->ptm);
}

void midi_tick(passport_midi *m, int cycles)
{
    ptm_tick(&m->ptm, cycles);
}

bool midi_irq(const passport_midi *m)
{
    return ptm_irq(&m->ptm) || acia_irq(m);
}

void midi_flush(passport_midi *m, uint64_t cycle)
{
    acia_update(m, cycle);
}

uint8_t midi_read(passport_midi *m, uint8_t reg, uint64_t cycle)
{
    if (reg < 8)
        return ptm_read(&m->ptm, reg);
    if (reg < 10)
        return acia_read(m, reg & 1, cycle);
    return 0;
}

void midi_write(passport_midi *m, uint8_t reg, uint8_t value, uint64_t cycle)
{
    if (reg < 8)
        ptm_write(&m->ptm, reg, value);
    else if (reg < 10)
        acia_write(m, reg & 1, value, cycle);
}
