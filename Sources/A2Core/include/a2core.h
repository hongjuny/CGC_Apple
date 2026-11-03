// Public API of the Apple II+ emulation core (used by the Swift front end).
#ifndef A2CORE_H
#define A2CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2_SCREEN_W 560
#define A2_SCREEN_H 384
#define A2_AUDIO_RATE 44100
#define A2_DSK_SIZE 143360      // 35 tracks x 16 sectors x 256 bytes
#define A2_NIB_SIZE 232960      // 35 tracks x 6656 nibbles

typedef struct apple2 apple2;

typedef enum {
    A2_MODEL_IIPLUS = 0,        // 48K + 16K language card, NMOS 6502
    A2_MODEL_IIE,               // 128K with extended 80-column card, NMOS 6502
    A2_MODEL_IIE_ENHANCED,      // as IIe, 65C02 and enhanced ROMs
} a2_model;

typedef enum {
    A2_MONITOR_COLOR = 0,
    A2_MONITOR_GREEN,
    A2_MONITOR_AMBER,
    A2_MONITOR_WHITE,
} a2_monitor;

typedef enum {
    A2_DISK_DOS_ORDER = 0,      // .dsk / .do
    A2_DISK_PRODOS_ORDER,       // .po
    A2_DISK_NIBBLE,             // .nib
    A2_DISK_WOZ,                // .woz (WOZ 1 or 2; saved as WOZ 2)
} a2_disk_format;

apple2 *a2_create(a2_model model);
void a2_destroy(apple2 *a2);
a2_model a2_get_model(const apple2 *a2);

// ROM images. System ROM: 12 KB $D000-$FFFF (II+) or 16 KB $C000-$FFFF (IIe).
// Character ROM: 2 KB (II+, 341-0036) or 4 KB (IIe video ROM).
// Disk II boot PROM: 256 bytes (341-0027). Returns false on a size mismatch.
bool a2_load_system_rom(apple2 *a2, const uint8_t *data, size_t len);
bool a2_load_char_rom(apple2 *a2, const uint8_t *data, size_t len);
bool a2_load_disk_rom(apple2 *a2, const uint8_t *data, size_t len);

void a2_power_cycle(apple2 *a2);  // cold start: clears RAM
void a2_reset(apple2 *a2);        // Ctrl-RESET

// Runs one video frame (17030 CPU cycles) and renders the framebuffer.
void a2_run_frame(apple2 *a2);
// Same, optionally skipping the framebuffer update (for running ahead fast).
void a2_run_frame_ex(apple2 *a2, bool render);
// Renders the framebuffer from the current video state.
void a2_render(apple2 *a2);
// RGBA8888 (R in the lowest byte), A2_SCREEN_W x A2_SCREEN_H.
const uint32_t *a2_framebuffer(const apple2 *a2);
void a2_set_monitor(apple2 *a2, a2_monitor mode);
void a2_set_scanlines(apple2 *a2, bool on);

// Moves up to max_frames stereo frames (interleaved left/right 16-bit samples)
// produced so far into out, which must hold 2 * max_frames samples.
size_t a2_take_audio(apple2 *a2, int16_t *out, size_t max_frames);
// Produces ratio x A2_AUDIO_RATE samples per emulated second (clamped to
// 0.98..1.02), so the front end can keep its output buffer level steady.
void a2_set_audio_rate_adjust(apple2 *a2, double ratio);

// Mockingboard (two AY-3-8913 PSGs) in slot 4: 0 or 1 (the default).
// Its first PSG plays left, its second right.
void a2_set_mockingboards(apple2 *a2, int count);
int a2_mockingboards(const apple2 *a2);

// AE memory cards, both modelled with 8 MB as in MAME. Disabled by default.
// RamWorks III replaces the IIe auxiliary card (64 KB banks; video uses bank 0).
// Enabling on a II+ or allocation failure returns false without changing the card.
bool a2_set_ramworks(apple2 *a2, bool enabled);
bool a2_ramworks(const apple2 *a2);
// RamFactor occupies slot 1; its firmware must be loaded before enabling it.
// Accepts one 8 KB ROM revision, not MAME's concatenated 32 KB region.
bool a2_load_ramfactor_rom(apple2 *a2, const uint8_t *data, size_t len);
// Enabling fails if the firmware is missing or allocation fails.
// Removing a card or power cycling discards its RAM; RESET preserves RAM.
bool a2_set_ramfactor(apple2 *a2, bool enabled);
bool a2_ramfactor(const apple2 *a2);

// Keyboard: 7-bit ASCII. The II+ has no lowercase; letters are upper-cased.
void a2_key_press(apple2 *a2, uint8_t ascii);
void a2_key_release(apple2 *a2);    // all keys up (IIe "any key down" flag)
bool a2_key_pending(const apple2 *a2);
// Buttons 0..2. On the IIe, 0 is Open Apple and 1 is Closed Apple.
void a2_set_button(apple2 *a2, int button, bool down);
void a2_set_paddle(apple2 *a2, int paddle, uint8_t value); // 0..3

// Disk II in slot 6. drive is 0 or 1.
bool a2_disk_insert(apple2 *a2, int drive, const uint8_t *data, size_t len,
                    a2_disk_format format, bool write_protected);
void a2_disk_eject(apple2 *a2, int drive);
bool a2_disk_present(const apple2 *a2, int drive);
bool a2_disk_dirty(const apple2 *a2, int drive);
// Clears the modified flag, after the image is saved or its changes are discarded.
void a2_disk_mark_clean(apple2 *a2, int drive);

// Hard disk in slot 7: a ProDOS block device with built-in firmware, so no ROM
// is needed. Accepts raw ProDOS-order images (.hdv, .po) up to 32 MB and
// ProDOS-order 2IMG (.2mg). While mounted, slot 7 boots before the Disk II.
// Insert fails on a bad image.
bool a2_hd_insert(apple2 *a2, const uint8_t *data, size_t len, bool write_protected);
void a2_hd_eject(apple2 *a2);
bool a2_hd_present(const apple2 *a2);
bool a2_hd_dirty(const apple2 *a2);
void a2_hd_mark_clean(apple2 *a2);
bool a2_hd_active(const apple2 *a2);    // accessed in the last few frames
// The whole image file, with any 2IMG header, as it should be saved.
size_t a2_hd_export_size(const apple2 *a2);
size_t a2_hd_export(apple2 *a2, uint8_t *out, size_t max);

// Mouse card in slot 2 (AppleMouse II firmware interface, own firmware, no ROM
// needed). The host pointer is given as a position over the Apple II screen,
// 0..1 from the top left, and mapped over the window the program clamps to.
bool a2_set_mouse(apple2 *a2, bool enabled);
bool a2_mouse_enabled(const apple2 *a2);
void a2_mouse_move(apple2 *a2, float x, float y);
void a2_mouse_button(apple2 *a2, bool down);

// SmartPort in slot 5, relayed to FujiNet-PC over TCP ("SP over SLIP"). The
// card listens on 127.0.0.1:port (FujiNet's default is 1985) and FujiNet-PC
// connects to it. Returns false if the port cannot be opened.
bool a2_set_smartport(apple2 *a2, bool enabled, int port);
int a2_smartport_state(const apple2 *a2);   // 0 off, 1 waiting for FujiNet, 2 connected
int a2_smartport_units(const apple2 *a2);   // devices FujiNet offers
bool a2_smartport_active(const apple2 *a2); // used in the last few frames
// Writes the (possibly modified) image back in its original format into out.
// a2_disk_export_size gives the buffer size needed. Returns the number of
// bytes written, or 0 on failure.
size_t a2_disk_export_size(const apple2 *a2, int drive);
size_t a2_disk_export(apple2 *a2, int drive, uint8_t *out, size_t max);
// Same, converting to another format (e.g. a .dsk saved as WOZ).
size_t a2_disk_export_size_as(const apple2 *a2, int drive, a2_disk_format format);
size_t a2_disk_export_as(apple2 *a2, int drive, a2_disk_format format, uint8_t *out, size_t max);
bool a2_disk_motor_on(const apple2 *a2);
int a2_disk_active_drive(const apple2 *a2);

// Side-effect-free memory read for debugging and tests.
uint8_t a2_peek(const apple2 *a2, uint16_t addr);
uint16_t a2_cpu_pc(const apple2 *a2);

#ifdef __cplusplus
}
#endif

#endif
