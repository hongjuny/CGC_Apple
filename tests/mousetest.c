// Mouse card (slot 2) test, run from 6502 code against the real ROMs:
//  1. the ID bytes programs look for, and the routine table at $C212-$C219
//  2. InitMouse, ClampMouse X 0..279 / Y 0..191, SetMouse (and a bad mode)
//  3. host pointer at (0.5, 0.25) with the button down, then ReadMouse:
//     X 140, Y 48, status button + moved
//  4. VBL interrupts: SetMouse $09 with an IRQ handler that calls ServeMouse
//     and counts; expects 60 a second
//  5. HomeMouse puts the position at the clamp minimum
// Usage: mousetest <romdir>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

enum { SET, SERVE, READ, CLEAR, POS, CLAMP, HOME, INIT };

static uint8_t *code;
static int pc;

static void emit(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    while (n--)
        code[pc++] = (uint8_t)va_arg(ap, int);
    va_end(ap);
}
static void store(uint8_t v, uint16_t addr) { emit(5, 0xA9, v, 0x8D, addr & 0xFF, addr >> 8); }
static void save_carry(uint8_t zp) { emit(5, 0xA9, 0x00, 0x2A, 0x85, zp); }
static void copy(uint16_t from, uint8_t zp) { emit(5, 0xAD, from & 0xFF, from >> 8, 0x85, zp); }

static apple2 *a2;

// LDX #$C2 : LDY #$20 : JSR $C2xx, with xx from the card's routine table
static void call(int fn)
{
    uint8_t offset = bus_read(a2, (uint16_t)(0xC212 + fn));
    emit(7, 0xA2, 0xC2, 0xA0, 0x20, 0x20, offset, 0xC2);
}
static void start(uint16_t at) { code = a2->ram[0] + at; pc = 0; }
static void finish(uint16_t at) { emit(3, 0x4C, (at + pc) & 0xFF, (at + pc) >> 8); }

static bool load(const char *dir, const char *name, bool (*loader)(apple2 *, const uint8_t *, size_t))
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    static uint8_t buf[0x4000];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return loader(a2, buf, n);
}

static void run_at(uint16_t at, int frames)
{
    // We jump in from wherever the ROM was waiting for a key; on the IIe that
    // is inside its internal $Cnxx firmware, so switch the slot ROMs back in.
    bus_write(a2, 0xC006, 0);
    a2->cpu.pc = at;
    for (int f = 0; f < frames; f++)
        a2_run_frame(a2);
}

static int check(const char *what, bool ok)
{
    printf("  %-40s %s\n", what, ok ? "PASS" : "FAIL");
    return !ok;
}

static int test(const char *romdir, a2_model model)
{
    static const char *names[][2] = {
        { "Apple2_Plus.rom", "Apple2_Video.rom" },
        { "Apple2e.rom", "Apple2e_Enhanced_Video.rom" },
        { "Apple2e_Enhanced.rom", "Apple2e_Enhanced_Video.rom" },
    };
    a2 = a2_create(model);
    if (!load(romdir, names[model][0], a2_load_system_rom) || !load(romdir, names[model][1], a2_load_char_rom)) {
        fprintf(stderr, "ROMs not found in %s\n", romdir);
        exit(2);
    }
    a2_set_mouse(a2, true);
    a2_power_cycle(a2);                     // no Disk II ROM: straight to BASIC
    for (int f = 0; f < 30; f++)
        a2_run_frame(a2);
    int failures = 0;
    uint8_t *zp = a2->ram[0];

    bus_write(a2, 0xC006, 0);               // slot ROMs in (see run_at)
    bool id = bus_read(a2, 0xC205) == 0x38 && bus_read(a2, 0xC207) == 0x18 &&
              bus_read(a2, 0xC20B) == 0x01 && bus_read(a2, 0xC20C) == 0x20 && bus_read(a2, 0xC2FB) == 0xD6;
    failures += check("ID bytes", id);

    // 2. init, clamp to the hi-res screen, turn on
    start(0x6000);
    emit(1, 0x78);                          // SEI
    call(INIT); save_carry(0x10);
    store(0, 0x478); store(0, 0x578); store(279 & 0xFF, 0x4F8); store(279 >> 8, 0x5F8);
    emit(2, 0xA9, 0x00); call(CLAMP);       // X 0..279
    store(0, 0x478); store(0, 0x578); store(191, 0x4F8); store(0, 0x5F8);
    emit(2, 0xA9, 0x01); call(CLAMP);       // Y 0..191
    emit(2, 0xA9, 0x01); call(SET); save_carry(0x11);   // mouse on
    emit(2, 0xA9, 0x10); call(SET); save_carry(0x12);   // invalid mode
    emit(2, 0xA9, 0x01); call(SET);
    finish(0x6000);
    run_at(0x6000, 2);
    failures += check("InitMouse, SetMouse ok; bad mode refused", zp[0x10] == 0 && zp[0x11] == 0 && zp[0x12] == 1);

    // 3. the host moves the pointer and presses the button
    a2_mouse_move(a2, 0.5f, 0.25f);
    a2_mouse_button(a2, true);
    start(0x6200);
    call(READ);
    copy(0x47A, 0x20); copy(0x57A, 0x21); copy(0x4FA, 0x22); copy(0x5FA, 0x23); copy(0x77A, 0x24);
    finish(0x6200);
    run_at(0x6200, 2);
    int x = zp[0x20] | zp[0x21] << 8, y = zp[0x22] | zp[0x23] << 8;
    failures += check("ReadMouse: X 140, Y 48 in the clamp", x == 140 && y == 48);
    failures += check("ReadMouse: button down, moved", (zp[0x24] & 0xE0) == 0xA0);
    a2_mouse_button(a2, false);

    // 4. VBL interrupts served by an IRQ handler
    start(0x6400);
    call(SERVE);
    emit(2, 0xB0, 0x06);                    // BCS +6 (not ours)
    emit(2, 0xE6, 0x06);                    // INC $06
    emit(2, 0xD0, 0x02);                    // BNE +2
    emit(2, 0xE6, 0x07);                    // INC $07
    emit(2, 0xA5, 0x45);                    // LDA $45 (the ROM saved A there)
    emit(1, 0x40);                          // RTI
    a2->ram[0][0x3FE] = 0x00;
    a2->ram[0][0x3FF] = 0x64;
    zp[0x06] = zp[0x07] = 0;
    start(0x6600);
    emit(2, 0xA9, 0x09); call(SET);         // on + VBL interrupt
    emit(1, 0x58);                          // CLI
    finish(0x6600);
    run_at(0x6600, 60);
    unsigned irqs = zp[0x06] | zp[0x07] << 8;
    failures += check("VBL interrupts: 60 in 60 frames", irqs >= 59 && irqs <= 61);
    printf("    (%u interrupts)\n", irqs);

    // 5. home
    start(0x6800);
    emit(1, 0x78);                          // SEI
    emit(2, 0xA9, 0x01); call(SET);         // interrupts off
    call(HOME);
    copy(0x47A, 0x20); copy(0x57A, 0x21); copy(0x4FA, 0x22); copy(0x5FA, 0x23);
    finish(0x6800);
    run_at(0x6800, 2);
    failures += check("HomeMouse: position at the clamp minimum",
                      zp[0x20] == 0 && zp[0x21] == 0 && zp[0x22] == 0 && zp[0x23] == 0);

    // 6. PositionMouse uses the same signed coordinates as ClampMouse.
    start(0x6A00);
    store(0x9C, 0x478); store(0xFF, 0x578); // min -100
    store(100, 0x4F8); store(0, 0x5F8);    // max 100
    emit(2, 0xA9, 0); call(CLAMP);
    emit(2, 0xA9, 1); call(CLAMP);
    store(0xCE, 0x47A); store(0xFF, 0x57A); // X -50
    store(0xB5, 0x4FA); store(0xFF, 0x5FA); // Y -75
    call(POS); call(READ);
    copy(0x47A, 0x20); copy(0x57A, 0x21); copy(0x4FA, 0x22); copy(0x5FA, 0x23);
    finish(0x6A00);
    run_at(0x6A00, 2);
    failures += check("PositionMouse: signed X/Y in the clamp",
                      (int16_t)(zp[0x20] | zp[0x21] << 8) == -50 &&
                      (int16_t)(zp[0x22] | zp[0x23] << 8) == -75);
    a2_destroy(a2);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir\n", argv[0]);
        return 2;
    }
    int failures = 0;
    printf("II+\n");
    failures += test(argv[1], A2_MODEL_IIPLUS);
    printf("enhanced IIe\n");
    failures += test(argv[1], A2_MODEL_IIE_ENHANCED);
    printf(failures ? "FAIL\n" : "all PASS\n");
    return failures ? 1 : 0;
}
