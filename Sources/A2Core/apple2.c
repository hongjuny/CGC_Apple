// Apple II+ / IIe machine: memory map and banking, soft switches, language
// card, keyboard, game I/O and speaker. Memory map and I/O decoding follow
// MAME's src/mame/apple/apple2.cpp and apple2e.cpp (BSD-3-Clause, R. Belmont).
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

// ---- memory map --------------------------------------------------------------

static void update_map(apple2 *a2)
{
    bool e = a2_is_iie(a2);
    uint8_t *main_ram = a2->ram[0], *aux_ram = a2->ram[1];
    if (a2->ramworks_extra && a2->ramworks_bank)
        aux_ram = a2->ramworks_extra + (a2->ramworks_bank - 1) * 0x10000;

    // $0000-$01FF and the language card follow ALTZP.
    uint8_t *zp = (e && a2->altzp) ? aux_ram : main_ram;
    for (int p = 0x00; p < 0x02; p++) {
        a2->rd_page[p] = zp + p * 256;
        a2->wr_page[p] = zp + p * 256;
    }

    uint8_t *rd_bank = (e && a2->ramrd) ? aux_ram : main_ram;
    uint8_t *wr_bank = (e && a2->ramwrt) ? aux_ram : main_ram;
    for (int p = 0x02; p < 0xC0; p++) {
        a2->rd_page[p] = rd_bank + p * 256;
        a2->wr_page[p] = wr_bank + p * 256;
    }

    // 80STORE: PAGE2 selects main/aux for text page 1 (and hi-res page 1 when HIRES is on).
    if (e && a2->store80) {
        uint8_t *bank = a2->page2 ? aux_ram : main_ram;
        for (int p = 0x04; p < 0x08; p++) {
            a2->rd_page[p] = bank + p * 256;
            a2->wr_page[p] = bank + p * 256;
        }
        if (a2->hires) {
            for (int p = 0x20; p < 0x40; p++) {
                a2->rd_page[p] = bank + p * 256;
                a2->wr_page[p] = bank + p * 256;
            }
        }
    }

    for (int p = 0xC0; p < 0xD0; p++) {
        a2->rd_page[p] = NULL;
        a2->wr_page[p] = NULL;
    }

    for (int p = 0xD0; p < 0x100; p++) {
        // Bank 1 of $D000 is stored at $C000 in the same memory bank.
        int ram_page = (p < 0xE0 && !a2->lc_bank2) ? p - 0x10 : p;
        uint8_t *ram = zp + ram_page * 256;
        a2->rd_page[p] = a2->lc_read_ram ? ram : a2->rom + (p - 0xC0) * 256;
        a2->wr_page[p] = a2->lc_write_enable ? ram : NULL;
    }
}

static uint8_t floating_bus(apple2 *a2)
{
    return a2->ram[0][video_scan_address(a2, a2->cpu.cycles)];
}

// ---- audio --------------------------------------------------------------------

static int16_t clamp16(float v)
{
    return (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
}

#define SPEAKER_GAIN 6000.0f

static double sample_time(const apple2 *a2, uint64_t cycle)
{
    return ((double)cycle - a2->audio_start) / a2->audio_cycles_per_sample;
}

// Brings the Mockingboard PSGs up to the current cycle.
static void mb_catch_up(apple2 *a2)
{
    for (int i = 0; i < a2->mb_count; i++)
        mb_sync(&a2->mb[i], a2->cpu.cycles, a2->blip_out, a2->audio_start, a2->audio_cycles_per_sample);
}

static void speaker_toggle(apple2 *a2)
{
    a2->speaker = !a2->speaker;
    float delta = a2->speaker ? 2 * SPEAKER_GAIN : -2 * SPEAKER_GAIN;
    double t = sample_time(a2, a2->cpu.cycles);
    blip_add(&a2->blip_out[0], t, delta);
    blip_add(&a2->blip_out[1], t, delta);
}

// Moves every sample whose band-limited steps are complete to the output queue.
static void audio_flush(apple2 *a2)
{
    mb_catch_up(a2);
    int n = (int)sample_time(a2, a2->cpu.cycles) - BLIP_TAPS;
    if (n <= 0)
        return;
    if (n > BLIP_SIZE)
        n = BLIP_SIZE;
    float buf[2][BLIP_SIZE];
    blip_read(&a2->blip_out[0], n, buf[0]);
    blip_read(&a2->blip_out[1], n, buf[1]);
    a2->audio_start += n * a2->audio_cycles_per_sample;

    for (int i = 0; i < n; i++) {
        int16_t out[2];
        for (int c = 0; c < 2; c++) {
            // DC blocker: the speaker cone relaxes when it stops being toggled,
            // and the PSG outputs are unipolar.
            float x = buf[c][i];
            float y = x - a2->dc_x1[c] + 0.995f * a2->dc_y1[c];
            a2->dc_x1[c] = x;
            a2->dc_y1[c] = y;
            out[c] = clamp16(y);
        }
        if (a2->audio_count < AUDIO_BUF_LEN) {
            size_t tail = (a2->audio_head + a2->audio_count) % AUDIO_BUF_LEN;
            a2->audio[tail * 2] = out[0];
            a2->audio[tail * 2 + 1] = out[1];
            a2->audio_count++;
        }
    }
}

// ---- I/O ----------------------------------------------------------------------

// Language card soft switches $C080-$C08F (Apple II Reference Manual / MAME a2bus RAMCARD16K).
static void lc_access(apple2 *a2, uint8_t reg, bool write)
{
    a2->lc_bank2 = !(reg & 0x08);
    a2->lc_read_ram = ((reg & 3) == 0) || ((reg & 3) == 3);
    if (reg & 1) {
        if (!write) {
            if (a2->lc_prewrite)
                a2->lc_write_enable = true;
            a2->lc_prewrite = true;
        } else {
            a2->lc_prewrite = false;
        }
    } else {
        a2->lc_prewrite = false;
        a2->lc_write_enable = false;
    }
    update_map(a2);
}

// IIe MMU/IOU switches written at $C000-$C00F.
static void iie_switch_write(apple2 *a2, uint8_t reg)
{
    bool on = reg & 1;
    switch (reg >> 1) {
    case 0: a2->store80 = on; break;
    case 1: a2->ramrd = on; break;
    case 2: a2->ramwrt = on; break;
    case 3: a2->intcxrom = on; break;
    case 4: a2->altzp = on; break;
    case 5: a2->slotc3rom = on; break;
    case 6: a2->col80 = on; break;
    case 7: a2->altchar = on; break;
    }
    update_map(a2);
}

// IIe status flags read at $C011-$C01F (bit 7), keyboard latch in the low bits.
static uint8_t iie_status_read(apple2 *a2, uint8_t reg)
{
    bool flag = false;
    switch (reg) {
    case 0x11: flag = a2->lc_bank2; break;
    case 0x12: flag = a2->lc_read_ram; break;
    case 0x13: flag = a2->ramrd; break;
    case 0x14: flag = a2->ramwrt; break;
    case 0x15: flag = a2->intcxrom; break;
    case 0x16: flag = a2->altzp; break;
    case 0x17: flag = a2->slotc3rom; break;
    case 0x18: flag = a2->store80; break;
    case 0x19: flag = !video_in_vbl(a2->cpu.cycles); break;
    case 0x1A: flag = a2->text; break;
    case 0x1B: flag = a2->mixed; break;
    case 0x1C: flag = a2->page2; break;
    case 0x1D: flag = a2->hires; break;
    case 0x1E: flag = a2->altchar; break;
    case 0x1F: flag = a2->col80; break;
    }
    return (uint8_t)((a2->key & 0x7F) | (flag ? 0x80 : 0));
}

static uint8_t io_access(apple2 *a2, uint16_t addr, bool write, uint8_t value)
{
    uint8_t reg = addr & 0xFF;
    bool e = a2_is_iie(a2);

    switch (reg >> 4) {
    case 0x0: // $C000 keyboard data; IIe: MMU switches on write
        if (write) {
            if (e)
                iie_switch_write(a2, reg);
            return 0;
        }
        return a2->key;
    case 0x1: // $C010 clear keyboard strobe; IIe: status flags
        if (reg == 0x10 || write) {
            uint8_t v = e ? (uint8_t)((a2->key & 0x7F) | (a2->any_key_down ? 0x80 : 0))
                          : (uint8_t)((a2->key & 0x7F) | (floating_bus(a2) & 0x80));
            a2->key &= 0x7F;
            return write ? 0 : v;
        }
        return e ? iie_status_read(a2, reg) : floating_bus(a2);
    case 0x2: // $C020 cassette output (not emulated)
        break;
    case 0x3: // $C030 speaker toggle
        speaker_toggle(a2);
        break;
    case 0x4: // $C040 utility strobe
        break;
    case 0x5: // $C050-$C057 video switches, $C058-$C05F annunciators
        switch (reg & 0x0F) {
        case 0x0: a2->text = false; break;
        case 0x1: a2->text = true; break;
        case 0x2: a2->mixed = false; break;
        case 0x3: a2->mixed = true; break;
        case 0x4: a2->page2 = false; update_map(a2); break;
        case 0x5: a2->page2 = true; update_map(a2); break;
        case 0x6: a2->hires = false; update_map(a2); break;
        case 0x7: a2->hires = true; update_map(a2); break;
        case 0xE: a2->dhires = true; break;    // AN3 off = double hi-res on (IIe)
        case 0xF: a2->dhires = false; break;
        default: break;
        }
        break;
    case 0x6: { // $C060 cassette in, $C061-$C063 buttons, $C064-$C067 paddle timers
        if (write)
            return 0;
        uint8_t fb = floating_bus(a2) & 0x7F;
        int r = reg & 0x07;
        if (r >= 1 && r <= 3)
            return (uint8_t)(fb | (a2->buttons[r - 1] ? 0x80 : 0));
        if (r >= 4) {
            // The 558 timer runs for roughly 11 cycles per paddle unit.
            uint64_t elapsed = a2->cpu.cycles - a2->paddle_trigger_cycle;
            bool running = elapsed < (uint64_t)a2->paddles[r - 4] * 11;
            return (uint8_t)(fb | (running ? 0x80 : 0));
        }
        return fb;
    }
    case 0x7: // $C070 paddle trigger and RamWorks bank-select aliases
        a2->paddle_trigger_cycle = a2->cpu.cycles;
        if (write && a2->ramworks_extra && (reg & 0x09) == 1) {
            a2->ramworks_bank = value & 0x7F;
            update_map(a2);
        }
        break;
    case 0x8: // language card
        lc_access(a2, reg, write);
        break;
    case 0xE: // slot 6: Disk II
        return disk2_io(&a2->disk, reg & 0x0F, write, value, a2->cpu.cycles);
    default:
        if ((reg >> 4) == 0xA && a2->mouse.enabled)   // $C0A0: slot 2 mouse
            return mouse_io(a2, reg & 0x0F, write, value);
        if ((reg >> 4) == 0xD && a2->sp.enabled)  // $C0D0: slot 5 SmartPort
            return sp_io(a2, reg & 0x0F, write);
        if ((reg >> 4) == 0xF && a2->hd.file)     // $C0F0: slot 7 hard disk
            return hd_io(a2, reg & 0x0F, write);
        if ((reg >> 4) == 0x9 && a2->ramfactor_ram)
            return ramfactor_io(a2, reg & 0x0F, write, value);
        break;
    }
    return write ? 0 : floating_bus(a2);
}

// $C100-$CFFF: peripheral card ROMs, or the IIe internal firmware.
static uint8_t slot_rom_read(apple2 *a2, uint16_t addr)
{
    int page = addr >> 8;
    bool card_c800 = a2->ramfactor_c800;
    if (addr == 0xCFFF)
        a2->ramfactor_c800 = false;
    if (a2_is_iie(a2)) {
        bool internal;
        if (page == 0xC3) {
            internal = a2->intcxrom || !a2->slotc3rom;
            if (internal)
                a2->intc8rom = true;   // the 80-column firmware claims $C800
        } else if (page >= 0xC8) {
            internal = a2->intcxrom || a2->intc8rom;
        } else {
            internal = a2->intcxrom;
        }
        uint8_t v = internal ? a2->rom[addr - 0xC000] : 0;
        if (addr == 0xCFFF)
            a2->intc8rom = false;
        if (internal)
            return v;
    }
    if (a2->ramfactor_ram) {
        if (page == 0xC1) {
            a2->ramfactor_c800 = true;
            return ramfactor_rom_read(a2, addr);
        }
        if (page >= 0xC8 && card_c800)
            return ramfactor_rom_read(a2, addr);
    }
    if (page == 0xC4 && a2->mb_count)
        return mb_read(&a2->mb[0], addr & 0xFF);
    if (page == 0xC2 && a2->mouse.enabled)
        return mouse_rom_read(a2, addr);
    if (page == 0xC5 && a2->sp.enabled)
        return sp_rom_read(a2, addr);
    if (page == 0xC7 && a2->hd.file)
        return hd_rom_read(a2, addr);
    if (page == 0xC6 && a2->disk.rom_loaded)
        return a2->disk.rom[addr & 0xFF];
    return floating_bus(a2);
}

static uint8_t mem_read(void *ctx, uint16_t addr)
{
    apple2 *a2 = ctx;
    const uint8_t *page = a2->rd_page[addr >> 8];
    if (page)
        return page[addr & 0xFF];
    if (addr < 0xC100)
        return io_access(a2, addr, false, 0);
    return slot_rom_read(a2, addr);
}

static void mem_write(void *ctx, uint16_t addr, uint8_t value)
{
    apple2 *a2 = ctx;
    uint8_t *page = a2->wr_page[addr >> 8];
    if (page) {
        page[addr & 0xFF] = value;
    } else if (addr < 0xC100) {
        io_access(a2, addr, true, value);
    } else if ((addr >> 8) == 0xC4 && a2->mb_count &&
               !(a2_is_iie(a2) && a2->intcxrom)) {
        mb_catch_up(a2);
        mb_write(&a2->mb[0], addr & 0xFF, value);
    } else if ((addr >> 8) == 0xC1 && a2->ramfactor_ram) {
        // INTCXROM gates reads; writes still reach the peripheral slot.
        a2->ramfactor_c800 = true;
    } else if (addr == 0xCFFF) {
        a2->ramfactor_c800 = false;
        if (a2_is_iie(a2))
            a2->intc8rom = false;
    }
}

uint8_t bus_read(apple2 *a2, uint16_t addr) { return mem_read(a2, addr); }
void bus_write(apple2 *a2, uint16_t addr, uint8_t value) { mem_write(a2, addr, value); }

// RamWorks bank switches follow MAME's bus/a2bus/a2eramworks3.cpp
// (BSD-3-Clause, R. Belmont); video always reads the original auxiliary bank.
bool a2_set_ramworks(apple2 *a2, bool enabled)
{
    if (enabled && !a2_is_iie(a2))
        return false;
    if (enabled == (a2->ramworks_extra != NULL))
        return true;
    if (enabled) {
        uint8_t *ram = calloc(RAMWORKS_EXTRA_SIZE, 1);
        if (!ram)
            return false;
        a2->ramworks_extra = ram;
    } else {
        free(a2->ramworks_extra);
        a2->ramworks_extra = NULL;
    }
    a2->ramworks_bank = 0;
    update_map(a2);
    return true;
}

bool a2_ramworks(const apple2 *a2)
{
    return a2->ramworks_extra != NULL;
}

// ---- lifecycle ----------------------------------------------------------------

apple2 *a2_create(a2_model model)
{
    apple2 *a2 = calloc(1, sizeof *a2);
    if (a2)
        sp_init(&a2->sp);
    if (!a2)
        return NULL;
    a2->model = model;
    a2->audio_cycles_per_sample = A2_CPU_HZ / A2_AUDIO_RATE;
    a2->monitor = A2_MONITOR_COLOR;
    for (int i = 0; i < 4; i++)
        a2->paddles[i] = 127;
    cpu_init(&a2->cpu, a2, mem_read, mem_write);
    a2->cpu.cmos = model == A2_MODEL_IIE_ENHANCED;
    a2->mb_count = 1;
    mb_reset(&a2->mb[0]);
    blip_clear(&a2->blip_out[0]);
    blip_clear(&a2->blip_out[1]);
    a2->audio_start = -BLIP_TAPS * a2->audio_cycles_per_sample;
    update_map(a2);
    return a2;
}

void a2_set_audio_rate_adjust(apple2 *a2, double ratio)
{
    if (ratio < 0.98) ratio = 0.98;
    if (ratio > 1.02) ratio = 1.02;
    // Keep the current sample position continuous while the rate changes.
    double t = sample_time(a2, a2->cpu.cycles);
    a2->audio_cycles_per_sample = A2_CPU_HZ / (A2_AUDIO_RATE * ratio);
    a2->audio_start = (double)a2->cpu.cycles - t * a2->audio_cycles_per_sample;
}

void a2_set_mockingboards(apple2 *a2, int count)
{
    mb_catch_up(a2);
    a2->mb_count = count < 0 ? 0 : count > 1 ? 1 : count;
    for (int i = 0; i < 1; i++) {
        mb_reset(&a2->mb[i]);
        a2->mb[i].ay_cycle = a2->cpu.cycles;
    }
    a2->cpu.irq_line = false;
}

int a2_mockingboards(const apple2 *a2)
{
    return a2->mb_count;
}

void a2_destroy(apple2 *a2)
{
    if (!a2)
        return;
    disk2_eject(&a2->disk, 0);
    disk2_eject(&a2->disk, 1);
    free(a2->ramworks_extra);
    free(a2->ramfactor_ram);
    a2_hd_eject(a2);
    sp_shutdown(&a2->sp);
    free(a2);
}

a2_model a2_get_model(const apple2 *a2)
{
    return a2->model;
}

bool a2_load_system_rom(apple2 *a2, const uint8_t *data, size_t len)
{
    size_t want = a2_is_iie(a2) ? 0x4000 : 0x3000;
    if (len != want)
        return false;
    memcpy(a2->rom + (0x4000 - want), data, len);
    a2->rom_loaded = true;
    return true;
}

bool a2_load_char_rom(apple2 *a2, const uint8_t *data, size_t len)
{
    size_t want = a2_is_iie(a2) ? 0x1000 : 0x800;
    if (len != want)
        return false;
    memcpy(a2->char_rom, data, len);
    return true;
}

bool a2_load_disk_rom(apple2 *a2, const uint8_t *data, size_t len)
{
    if (len != sizeof a2->disk.rom)
        return false;
    memcpy(a2->disk.rom, data, len);
    a2->disk.rom_loaded = true;
    return true;
}

static void reset_switches(apple2 *a2)
{
    a2->ramworks_bank = 0;
    ramfactor_reset(a2);
    if (a2_is_iie(a2)) {
        // All MMU switches off and the built-in language card reset (Sather, Fig 5.13).
        a2->store80 = a2->ramrd = a2->ramwrt = a2->intcxrom = false;
        a2->altzp = a2->slotc3rom = a2->intc8rom = false;
        a2->col80 = a2->altchar = false;
        a2->page2 = a2->hires = false;
        a2->lc_read_ram = false;
        a2->lc_bank2 = true;
        a2->lc_prewrite = false;
        a2->lc_write_enable = true;
    }
    update_map(a2);
}

void a2_power_cycle(apple2 *a2)
{
    // DRAM powers up with a stripe pattern; the Autostart ROM only needs it to
    // not look like a valid warm-start vector ($3F4 power-up byte).
    for (int b = 0; b < 2; b++)
        for (size_t i = 0; i < sizeof a2->ram[b]; i++)
            a2->ram[b][i] = (i & 2) ? 0xFF : 0x00;
    // The language card RAM ($D000-$FFFF, and bank 1 kept at $C000) starts cleared.
    for (int b = 0; b < 2; b++)
        memset(a2->ram[b] + 0xC000, 0, 0x4000);
    if (a2->ramworks_extra)
        memset(a2->ramworks_extra, 0, RAMWORKS_EXTRA_SIZE);
    if (a2->ramfactor_ram)
        memset(a2->ramfactor_ram, 0xFF, EXPANSION_RAM_SIZE);
    a2->lc_read_ram = false;
    a2->lc_write_enable = true;
    a2->lc_prewrite = false;
    a2->lc_bank2 = true;
    a2->text = true;
    a2->mixed = a2->page2 = a2->hires = a2->dhires = false;
    a2->key = 0;
    a2->any_key_down = false;
    a2->speaker = false;
    disk2_power_on(&a2->disk);
    bool cmos = a2->cpu.cmos;
    cpu_init(&a2->cpu, a2, mem_read, mem_write);
    a2->cpu.cmos = cmos;
    reset_switches(a2);
    cpu_reset(&a2->cpu);
    for (int i = 0; i < 1; i++) {
        mb_reset(&a2->mb[i]);
        a2->mb[i].ay_cycle = a2->cpu.cycles;
        a2->mb[i].level[0] = a2->mb[i].level[1] = 0;
    }
    mouse_reset(a2);
    // The CPU cycle counter restarts, so the audio timeline does too.
    a2->audio_start = (double)a2->cpu.cycles - BLIP_TAPS * a2->audio_cycles_per_sample;
    for (int c = 0; c < 2; c++) {
        blip_clear(&a2->blip_out[c]);
        a2->dc_x1[c] = a2->dc_y1[c] = 0;
    }
}

void a2_reset(apple2 *a2)
{
    // RESET on the II+ does not touch the video switches; the ROM sets them.
    disk2_reset(&a2->disk);
    reset_switches(a2);
    mb_catch_up(a2);
    for (int i = 0; i < 1; i++)
        mb_reset(&a2->mb[i]);   // the cards' VIAs share the bus RESET line
    sp_reset(a2);
    mouse_reset(a2);
    a2->cpu.irq_line = false;
    cpu_reset(&a2->cpu);
}

void a2_run_frame(apple2 *a2)
{
    a2_run_frame_ex(a2, true);
}

void a2_render(apple2 *a2)
{
    video_render(a2);
}

void a2_run_frame_ex(apple2 *a2, bool render)
{
    uint64_t end = a2->cpu.cycles + A2_CYCLES_PER_FRAME;
    mouse_vbl(a2);                  // the frame starts at vertical blank
    if (a2->mb_count > 0 || a2->mouse.enabled) {
        // Cards that interrupt: the Mockingboard's VIA timers run with the CPU,
        // and the mouse card raises its line at vertical blank.
        while (a2->cpu.cycles < end) {
            int cycles = cpu_step(&a2->cpu);
            bool irq = mouse_irq(a2);
            if (a2->mb_count > 0) {
                mb_tick(&a2->mb[0], cycles);
                irq |= mb_irq(&a2->mb[0]);
            }
            a2->cpu.irq_line = irq;
        }
    } else {
        while (a2->cpu.cycles < end)
            cpu_step(&a2->cpu);
    }
    sp_poll(a2);
    audio_flush(a2);
    a2->frame_count++;
    if (render)
        video_render(a2);
}

const uint32_t *a2_framebuffer(const apple2 *a2)
{
    return a2->fb;
}

void a2_set_monitor(apple2 *a2, a2_monitor mode)
{
    a2->monitor = mode;
}

void a2_set_scanlines(apple2 *a2, bool on)
{
    a2->scanlines = on;
}

size_t a2_take_audio(apple2 *a2, int16_t *out, size_t max_frames)
{
    size_t n = a2->audio_count < max_frames ? a2->audio_count : max_frames;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = a2->audio[a2->audio_head * 2];
        out[i * 2 + 1] = a2->audio[a2->audio_head * 2 + 1];
        a2->audio_head = (a2->audio_head + 1) % AUDIO_BUF_LEN;
    }
    a2->audio_count -= n;
    return n;
}

// ---- input --------------------------------------------------------------------

void a2_key_press(apple2 *a2, uint8_t ascii)
{
    ascii &= 0x7F;
    if (!a2_is_iie(a2) && ascii >= 'a' && ascii <= 'z')
        ascii -= 0x20;
    a2->key = ascii | 0x80;
    a2->any_key_down = true;
}

void a2_key_release(apple2 *a2)
{
    a2->any_key_down = false;
}

bool a2_key_pending(const apple2 *a2)
{
    return (a2->key & 0x80) != 0;
}

void a2_set_button(apple2 *a2, int button, bool down)
{
    if (button >= 0 && button < 3)
        a2->buttons[button] = down;
}

void a2_set_paddle(apple2 *a2, int paddle, uint8_t value)
{
    if (paddle >= 0 && paddle < 4)
        a2->paddles[paddle] = value;
}

// ---- disks --------------------------------------------------------------------

bool a2_disk_insert(apple2 *a2, int drive, const uint8_t *data, size_t len,
                    a2_disk_format format, bool write_protected)
{
    return disk2_insert(&a2->disk, drive, data, len, format, write_protected);
}

void a2_disk_eject(apple2 *a2, int drive)
{
    disk2_eject(&a2->disk, drive);
}

bool a2_disk_present(const apple2 *a2, int drive)
{
    return (drive == 0 || drive == 1) && a2->disk.drive[drive].present;
}

bool a2_disk_dirty(const apple2 *a2, int drive)
{
    return a2_disk_present(a2, drive) && a2->disk.drive[drive].dirty;
}

void a2_disk_mark_clean(apple2 *a2, int drive)
{
    if (drive == 0 || drive == 1)
        a2->disk.drive[drive].dirty = false;
}

size_t a2_disk_export_size_as(const apple2 *a2, int drive, a2_disk_format format)
{
    return disk2_export_size(&a2->disk, drive, format);
}

size_t a2_disk_export_as(apple2 *a2, int drive, a2_disk_format format, uint8_t *out, size_t max)
{
    return disk2_export(&a2->disk, drive, format, out, max);
}

size_t a2_disk_export_size(const apple2 *a2, int drive)
{
    if (!a2_disk_present(a2, drive))
        return 0;
    return a2_disk_export_size_as(a2, drive, a2->disk.drive[drive].format);
}

size_t a2_disk_export(apple2 *a2, int drive, uint8_t *out, size_t max)
{
    if (!a2_disk_present(a2, drive))
        return 0;
    return a2_disk_export_as(a2, drive, a2->disk.drive[drive].format, out, max);
}

bool a2_disk_motor_on(const apple2 *a2)
{
    return disk2_spinning(&a2->disk, a2->cpu.cycles);
}

int a2_disk_active_drive(const apple2 *a2)
{
    return a2->disk.active;
}

// ---- debugging ----------------------------------------------------------------

uint8_t a2_peek(const apple2 *a2, uint16_t addr)
{
    const uint8_t *page = a2->rd_page[addr >> 8];
    return page ? page[addr & 0xFF] : 0;
}

uint16_t a2_cpu_pc(const apple2 *a2)
{
    return a2->cpu.pc;
}
