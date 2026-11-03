// State shared between the core's translation units (not part of the public API).
#ifndef A2INTERNAL_H
#define A2INTERNAL_H

#include "a2core.h"
#include "cpu6502.h"

#define A2_CYCLES_PER_LINE   65
#define A2_LINES_PER_FRAME   262
#define A2_CYCLES_PER_FRAME  (A2_CYCLES_PER_LINE * A2_LINES_PER_FRAME)  // 17030
#define A2_CPU_HZ            1020484.0

#define DISK_TRACKS     35          // tracks in .dsk/.nib images
#define DISK_TRACK_LEN  6656        // nibbles per .nib track
#define DISK_QTRACKS    160         // quarter-track positions (WOZ TMAP)
#define AUDIO_BUF_LEN   8192

// A disk is a set of bit-stream tracks addressed through a quarter-track map,
// as in the WOZ format; sector and nibble images are converted on insert.
typedef struct {
    bool present;
    bool write_protected;
    bool dirty;
    a2_disk_format format;          // format the image came from (and is saved in)
    int halftrack;                  // head position, 0..79
    uint32_t bit_pos;               // head position within the current track

    uint8_t tmap[DISK_QTRACKS];     // quarter track -> track index, 0xFF = no track
    uint8_t *bits[DISK_QTRACKS];    // track bit streams, MSB first
    uint32_t bit_count[DISK_QTRACKS];
    int track_count;

    uint8_t woz_info[60];           // INFO chunk of a WOZ image (kept when saving)
    uint8_t *orig;                  // original sector image, used where tracks no longer decode
    size_t orig_len;
} disk_drive;

typedef struct {
    bool rom_loaded;
    uint8_t rom[256];
    disk_drive drive[2];
    int active;                     // selected drive
    bool motor;                     // motor-on switch state
    uint64_t motor_off_cycle;       // when the switch was turned off
    uint64_t now;                   // CPU cycle of the latest access
    bool q6, q7;
    uint8_t phases;                 // stepper magnet bitmask

    uint64_t bit_clock;             // bit cells processed so far (CPU cycles / 4)
    uint8_t shift;                  // read shift register
    uint8_t held;                   // last complete byte, still visible for `hold` cells
    int hold;
    uint8_t zero_window;            // last raw bits, for MC3470 noise on long zero runs
    uint32_t rng;
    uint8_t write_byte;             // write shift register
    int write_bits_out;             // bits of write_byte already written
} disk2;

#define BLIP_TAPS 16
#define BLIP_SIZE 4096              // samples buffered between reads

// Band-limited step synthesis buffer for one audio channel (audio.c).
typedef struct {
    float buf[BLIP_SIZE + BLIP_TAPS];
    double integrator;
} blip;

typedef struct {
    uint8_t reg[16];
    uint8_t address;                // latched register number
    unsigned tone_count[3];
    uint8_t tone_out[3];
    unsigned noise_count, env_count;
    uint32_t lfsr;
    int env_step;
    uint8_t env_attack;
    bool env_hold, env_alternate, env_holding;
} ay8913;

typedef struct {
    uint8_t orb, ora, ddrb, ddra, acr, pcr, ifr, ier, sr;
    int32_t t1, t2;                 // counters; negative = underflowed this tick
    uint16_t t1_latch;
    uint8_t t2_latch_lo;
    bool t1_fired, t2_fired;        // one-shot interrupt already raised
} via6522;

typedef struct {
    via6522 via[2];
    ay8913 ay[2];
    uint8_t psg_latch[2];           // PSG data read back onto port A
    uint64_t ay_cycle;              // CPU cycle the PSGs have been run up to
    float level[2];                 // current output level of each PSG
} mockingboard;

typedef struct {
    uint8_t cr[3];                  // control registers 1-3
    uint16_t latch[3];
    int32_t remaining[3];           // cycles until each timer next times out
    bool fired[3];                  // one-shot already signalled
    uint8_t status, status_read;    // IRQ flags; flags seen by a status read
    uint8_t msb_buffer, lsb_buffer;
    int prescale;                   // timer 3 divide-by-8 remainder
} ptm6840;

typedef struct {
    ptm6840 ptm;
    uint8_t acia_control;
    bool acia_reset;                // master reset held
    uint8_t tdr;                    // transmit data register
    bool tdr_full;
    uint64_t tdr_cycle;             // when tdr was written
    uint64_t shifter_free;          // cycle the transmit shifter becomes idle
    uint8_t out[4096];              // MIDI bytes sent, waiting for the front end
    size_t out_count;
} passport_midi;

struct apple2 {
    a2_model model;
    cpu6502 cpu;

    // Bank 0 is main memory, bank 1 the IIe auxiliary (80-column card) memory.
    // $C000-$CFFF of each bank holds language-card $D000 bank 1, which is
    // never addressable there directly.
    uint8_t ram[2][0x10000];
    uint8_t rom[0x4000];            // $C000-$FFFF (the II+ uses only $D000-$FFFF)
    uint8_t char_rom[0x1000];
    bool rom_loaded;

    // Page tables rebuilt whenever a banking switch changes. NULL entries are
    // handled by the I/O and slot ROM decoding (reads) or ignored (writes).
    const uint8_t *rd_page[256];
    uint8_t *wr_page[256];

    // Language card (slot 0 card on the II+, built in on the IIe)
    bool lc_read_ram, lc_write_enable, lc_prewrite, lc_bank2;

    // IIe MMU / IOU switches
    bool store80, ramrd, ramwrt, intcxrom, altzp, slotc3rom, intc8rom;
    bool col80, altchar, dhires;

    // video soft switches
    bool text, mixed, page2, hires;
    unsigned frame_count;

    // keyboard and game I/O
    uint8_t key;                    // bit 7 = strobe
    bool any_key_down;
    bool buttons[3];
    uint8_t paddles[4];
    uint64_t paddle_trigger_cycle;

    // speaker / audio
    bool speaker;
    double audio_cycles_per_sample;
    double audio_start;             // CPU cycle of sample 0 in the blip buffers
    blip blip_out[2];               // left, right
    float dc_x1[2], dc_y1[2];       // DC-blocking filter state, left/right
    int16_t audio[AUDIO_BUF_LEN * 2];   // interleaved stereo frames
    size_t audio_head, audio_count;     // in frames

    disk2 disk;

    int mb_count;                   // Mockingboards installed: 0, 1 (slot 4) or 2 (slots 4 and 5)
    mockingboard mb[2];

    int midi_slot;                  // Passport MIDI card slot, 0 = none
    passport_midi midi;

    a2_monitor monitor;
    bool scanlines;
    uint32_t fb[A2_SCREEN_W * A2_SCREEN_H];
};

static inline bool a2_is_iie(const apple2 *a2)
{
    return a2->model != A2_MODEL_IIPLUS;
}

// disk2.c
void disk2_power_on(disk2 *d);
void disk2_reset(disk2 *d);
uint8_t disk2_io(disk2 *d, uint8_t reg, bool write, uint8_t value, uint64_t cycle);
bool disk2_spinning(const disk2 *d, uint64_t cycle);
bool disk2_insert(disk2 *d, int drive, const uint8_t *data, size_t len, a2_disk_format fmt, bool wp);
void disk2_eject(disk2 *d, int drive);
size_t disk2_export_size(const disk2 *d, int drive, a2_disk_format fmt);
size_t disk2_export(disk2 *d, int drive, a2_disk_format fmt, uint8_t *out, size_t max);

// audio.c
void blip_clear(blip *b);
void blip_add(blip *b, double t, float delta);
void blip_read(blip *b, int n, float *out);

// mockingboard.c
void mb_reset(mockingboard *mb);
void mb_tick(mockingboard *mb, int cycles);
bool mb_irq(const mockingboard *mb);
// Runs both PSGs up to `cycle`, sending their level changes to the blip
// buffers (left PSG left, right PSG right). Sample time = (cycle - start) / cps.
void mb_sync(mockingboard *mb, uint64_t cycle, blip *out, double start, double cps);
uint8_t mb_read(mockingboard *mb, uint8_t offset);
void mb_write(mockingboard *mb, uint8_t offset, uint8_t value);

// midi.c
void midi_power_on(passport_midi *m);
void midi_reset(passport_midi *m);
void midi_tick(passport_midi *m, int cycles);
bool midi_irq(const passport_midi *m);
void midi_flush(passport_midi *m, uint64_t cycle);
uint8_t midi_read(passport_midi *m, uint8_t reg, uint64_t cycle);
void midi_write(passport_midi *m, uint8_t reg, uint8_t value, uint64_t cycle);

// video.c
void video_render(apple2 *a2);
uint16_t video_scan_address(const apple2 *a2, uint64_t cycle);
bool video_in_vbl(uint64_t cycle);

#endif
