// Sweet Micro Systems Mockingboard (sound part): two 6522 VIAs, each driving
// an AY-3-8913 PSG. VIA 1 ($Cn00-$Cn7F) feeds the left PSG, VIA 2
// ($Cn80-$CnFF) the right one. Port A is the PSG data bus; port B bits 0-2
// are BC1, BDIR and /RESET. Card wiring follows MAME's
// src/devices/bus/a2bus/a2mockingboard.cpp (BSD-3-Clause, R. Belmont).
// Speech chips (SC-01, SSI-263) are not emulated.
#include <string.h>
#include "a2internal.h"

#define AY_PRESCALE 8   // tone/noise/envelope counters step every 8 PSG clocks

// Normalised output levels of the AY-3-8910 DAC (logarithmic, ~3 dB per step).
static const float ay_level[16] = {
    0.0000f, 0.0106f, 0.0150f, 0.0222f, 0.0320f, 0.0466f, 0.0665f, 0.1039f,
    0.1237f, 0.1986f, 0.2803f, 0.3548f, 0.4702f, 0.6030f, 0.7530f, 1.0000f,
};

// ---- AY-3-8913 ----------------------------------------------------------------

static void ay_reset(ay8913 *ay)
{
    memset(ay->reg, 0, sizeof ay->reg);
    memset(ay->tone_count, 0, sizeof ay->tone_count);
    memset(ay->tone_out, 0, sizeof ay->tone_out);
    ay->noise_count = ay->env_count = 0;
    ay->lfsr = 1;
    ay->env_step = 0;
    ay->env_attack = 0;
    ay->env_hold = ay->env_alternate = ay->env_holding = true;
    ay->address = 0;
}

static void ay_write(ay8913 *ay, uint8_t value)
{
    static const uint8_t mask[16] = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
    };
    if (ay->address > 0x0F)
        return;
    int r = ay->address;
    ay->reg[r] = value & mask[r];
    if (r == 13) {
        // Writing the envelope shape restarts the envelope (MAME ay8910 logic).
        ay->env_attack = (value & 0x04) ? 0x0F : 0x00;
        if (!(value & 0x08)) {
            ay->env_hold = true;
            ay->env_alternate = ay->env_attack != 0;
        } else {
            ay->env_hold = value & 0x01;
            ay->env_alternate = value & 0x02;
        }
        ay->env_step = 0x0F;
        ay->env_holding = false;
        ay->env_count = 0;
    }
}

static uint8_t ay_read(const ay8913 *ay)
{
    return ay->address <= 0x0F ? ay->reg[ay->address] : 0xFF;
}

// One prescaled step (8 PSG clocks); returns the summed output of the three channels, 0..3.
static float ay_step(ay8913 *ay)
{
    for (int ch = 0; ch < 3; ch++) {
        unsigned period = ay->reg[ch * 2] | ((ay->reg[ch * 2 + 1] & 0x0F) << 8);
        if (period == 0)
            period = 1;
        if (++ay->tone_count[ch] >= period) {
            ay->tone_count[ch] = 0;
            ay->tone_out[ch] ^= 1;      // full wave = 16 * period PSG clocks
        }
    }

    unsigned noise_period = ay->reg[6] & 0x1F;
    if (noise_period == 0)
        noise_period = 1;
    if (++ay->noise_count >= noise_period * 2) {
        ay->noise_count = 0;
        // 17-bit LFSR, feedback from bits 0 and 3.
        uint32_t bit = (ay->lfsr ^ (ay->lfsr >> 3)) & 1;
        ay->lfsr = (ay->lfsr >> 1) | (bit << 16);
    }

    unsigned env_period = ay->reg[11] | (ay->reg[12] << 8);
    if (env_period == 0)
        env_period = 1;
    if (++ay->env_count >= env_period * 2) {
        ay->env_count = 0;
        if (!ay->env_holding) {
            ay->env_step--;
            if (ay->env_step < 0) {
                if (ay->env_hold) {
                    if (ay->env_alternate)
                        ay->env_attack ^= 0x0F;
                    ay->env_holding = true;
                    ay->env_step = 0;
                } else {
                    if (ay->env_alternate && (ay->env_step & 0x10))
                        ay->env_attack ^= 0x0F;
                    ay->env_step &= 0x0F;
                }
            }
        }
    }
    uint8_t env_volume = (uint8_t)((ay->env_step ^ ay->env_attack) & 0x0F);

    float out = 0;
    uint8_t mixer = ay->reg[7];
    int noise = ay->lfsr & 1;
    for (int ch = 0; ch < 3; ch++) {
        int tone_on = ay->tone_out[ch] | ((mixer >> ch) & 1);
        int noise_on = noise | ((mixer >> (ch + 3)) & 1);
        if (tone_on && noise_on) {
            uint8_t amp = ay->reg[8 + ch];
            out += ay_level[(amp & 0x10) ? env_volume : (amp & 0x0F)];
        }
    }
    return out;
}

// ---- 6522 VIA -----------------------------------------------------------------

enum { VIA_IFR_T2 = 0x20, VIA_IFR_T1 = 0x40 };

static void via_reset(via6522 *v)
{
    // RESET clears all registers except the timers, latches and shift register.
    v->orb = v->ora = v->ddrb = v->ddra = 0;
    v->acr = v->pcr = v->ifr = v->ier = 0;
    v->t1_fired = v->t2_fired = true;
}

static uint8_t via_port_a(const via6522 *v) { return (uint8_t)((v->ora & v->ddra) | ~v->ddra); }
static uint8_t via_port_b(const via6522 *v) { return (uint8_t)((v->orb & v->ddrb) | ~v->ddrb); }

static void via_tick(via6522 *v, int cycles)
{
    v->t1 -= cycles;
    while (v->t1 < 0) {
        if (v->acr & 0x40) {
            v->ifr |= VIA_IFR_T1;            // free-running: interrupt every latch+2 cycles
            v->t1 += (int32_t)v->t1_latch + 2;
        } else {
            if (!v->t1_fired)
                v->ifr |= VIA_IFR_T1;        // one-shot
            v->t1_fired = true;
            v->t1 += 0x10000;
        }
    }
    v->t2 -= cycles;
    while (v->t2 < 0) {
        if (!v->t2_fired)
            v->ifr |= VIA_IFR_T2;
        v->t2_fired = true;
        v->t2 += 0x10000;
    }
}

static bool via_irq(const via6522 *v)
{
    return (v->ifr & v->ier & 0x7F) != 0;
}

// ---- card ---------------------------------------------------------------------

// Port B drives the PSG bus control lines (MAME a2bus_ayboard_device::via_psg_ctrl).
static void psg_bus(mockingboard *mb, int n)
{
    via6522 *v = &mb->via[n];
    ay8913 *ay = &mb->ay[n];
    uint8_t ctrl = via_port_b(v);
    if (!(ctrl & 0x04)) {
        ay_reset(ay);
        return;
    }
    switch (ctrl & 3) {
    case 1: mb->psg_latch[n] = ay_read(ay); break;      // BC1: read PSG
    case 2: ay_write(ay, via_port_a(v)); break;          // BDIR: write PSG
    case 3: ay->address = via_port_a(v); break;          // BDIR+BC1: latch address
    default: break;
    }
}

void mb_reset(mockingboard *mb)
{
    for (int i = 0; i < 2; i++) {
        via_reset(&mb->via[i]);
        ay_reset(&mb->ay[i]);
        mb->psg_latch[i] = 0xFF;
    }
}

// Output scale per PSG: three channels at full volume give 10500, so two
// cards (two PSGs per side) plus the speaker stay below clipping.
#define PSG_GAIN 3500.0f

void mb_sync(mockingboard *mb, uint64_t cycle, blip *out, double start, double cps)
{
    if (cycle > mb->ay_cycle + 1000000)
        mb->ay_cycle = cycle - 1000000;   // don't replay a long silent stretch
    while (mb->ay_cycle + AY_PRESCALE <= cycle) {
        mb->ay_cycle += AY_PRESCALE;
        double t = ((double)mb->ay_cycle - start) / cps;
        for (int n = 0; n < 2; n++) {
            float level = ay_step(&mb->ay[n]) * PSG_GAIN;
            if (level != mb->level[n]) {
                blip_add(&out[n], t, level - mb->level[n]);
                mb->level[n] = level;
            }
        }
    }
}

void mb_tick(mockingboard *mb, int cycles)
{
    via_tick(&mb->via[0], cycles);
    via_tick(&mb->via[1], cycles);
}

bool mb_irq(const mockingboard *mb)
{
    return via_irq(&mb->via[0]) || via_irq(&mb->via[1]);
}

uint8_t mb_read(mockingboard *mb, uint8_t offset)
{
    int n = offset >= 0x80;
    via6522 *v = &mb->via[n];
    switch (offset & 0x0F) {
    case 0x0: return via_port_b(v);
    case 0x1: case 0xF: return (uint8_t)((v->ora & v->ddra) | (mb->psg_latch[n] & ~v->ddra));
    case 0x2: return v->ddrb;
    case 0x3: return v->ddra;
    case 0x4: v->ifr &= (uint8_t)~VIA_IFR_T1; return (uint8_t)v->t1;
    case 0x5: return (uint8_t)(v->t1 >> 8);
    case 0x6: return (uint8_t)v->t1_latch;
    case 0x7: return (uint8_t)(v->t1_latch >> 8);
    case 0x8: v->ifr &= (uint8_t)~VIA_IFR_T2; return (uint8_t)v->t2;
    case 0x9: return (uint8_t)(v->t2 >> 8);
    case 0xA: return v->sr;
    case 0xB: return v->acr;
    case 0xC: return v->pcr;
    case 0xD: return (uint8_t)(v->ifr | (via_irq(v) ? 0x80 : 0));
    case 0xE: return (uint8_t)(v->ier | 0x80);
    }
    return 0;
}

// The caller runs mb_sync up to the current cycle first, so PSG changes take effect from now on.
void mb_write(mockingboard *mb, uint8_t offset, uint8_t value)
{
    int n = offset >= 0x80;
    via6522 *v = &mb->via[n];
    switch (offset & 0x0F) {
    case 0x0: v->orb = value; psg_bus(mb, n); break;
    case 0x1: case 0xF: v->ora = value; break;
    case 0x2: v->ddrb = value; psg_bus(mb, n); break;
    case 0x3: v->ddra = value; break;
    case 0x4: case 0x6: v->t1_latch = (uint16_t)((v->t1_latch & 0xFF00) | value); break;
    case 0x5:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FF) | (value << 8));
        v->t1 = v->t1_latch;
        v->t1_fired = false;
        v->ifr &= (uint8_t)~VIA_IFR_T1;
        break;
    case 0x7:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FF) | (value << 8));
        v->ifr &= (uint8_t)~VIA_IFR_T1;
        break;
    case 0x8: v->t2_latch_lo = value; break;
    case 0x9:
        v->t2 = v->t2_latch_lo | (value << 8);
        v->t2_fired = false;
        v->ifr &= (uint8_t)~VIA_IFR_T2;
        break;
    case 0xA: v->sr = value; break;
    case 0xB: v->acr = value; break;
    case 0xC: v->pcr = value; break;
    case 0xD: v->ifr &= (uint8_t)~(value & 0x7F); break;
    case 0xE:
        if (value & 0x80)
            v->ier |= value & 0x7F;
        else
            v->ier &= (uint8_t)~(value & 0x7F);
        break;
    }
}
