// AE memory cards through the machine bus, including MMU/video and ROM arbitration.
// clang -O2 -ISources/A2Core/include -ISources/A2Core -o /tmp/memorytest tests/memorytest.c Sources/A2Core/*.c
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

static uint8_t read8(apple2 *a, uint16_t addr) { return a->cpu.read(a, addr); }
static void write8(apple2 *a, uint16_t addr, uint8_t value) { a->cpu.write(a, addr, value); }

static void ramworks(a2_model model)
{
    apple2 *a = a2_create(model);
    a2_power_cycle(a);
    assert(!a2_ramworks(a));
    assert(a2_set_ramworks(a, true));
    write8(a, 0xC003, 0); // RAMRD
    write8(a, 0xC005, 0); // RAMWRT
    write8(a, 0xC009, 0); // ALTZP, including language-card RAM
    read8(a, 0xC083); read8(a, 0xC083);
    const uint16_t addresses[] = {0x20, 0x100, 0x200, 0x400, 0x2000, 0xD000, 0xE000};
    for (int bank = 0; bank < 128; bank++) {
        write8(a, 0xC073, (uint8_t)bank);
        for (size_t i = 0; i < sizeof addresses / sizeof *addresses; i++)
            write8(a, addresses[i], (uint8_t)(bank ^ (i + 1)));
        read8(a, 0xC08B); read8(a, 0xC08B); // Language-card bank 1 within this auxiliary bank.
        write8(a, 0xD000, (uint8_t)(bank ^ 0x80));
        read8(a, 0xC083);
    }
    for (int bank = 0; bank < 128; bank++) {
        write8(a, 0xC073, (uint8_t)bank);
        for (size_t i = 0; i < sizeof addresses / sizeof *addresses; i++)
            assert(read8(a, addresses[i]) == (uint8_t)(bank ^ (i + 1)));
        read8(a, 0xC08B);
        assert(read8(a, 0xD000) == (uint8_t)(bank ^ 0x80));
        read8(a, 0xC083);
    }
    // All four bank-select aliases mask bit 7; reads never change the bank.
    for (int reg = 0x71; reg <= 0x77; reg += 2) {
        write8(a, (uint16_t)(0xC000 + reg), 0x81);
        assert(read8(a, 0x200) == (1 ^ 3));
    }
    read8(a, 0xC071);
    write8(a, 0xC079, 7);
    assert(a->ramworks_bank == 1);
    assert(a2_set_ramworks(a, true));
    assert(a->ramworks_bank == 1);

    // RAMRD/RAMWRT remain independent of each other and of the video bank.
    write8(a, 0xC008, 0);
    write8(a, 0xC002, 0);
    write8(a, 0xC004, 0);
    write8(a, 0x200, 0xA5);
    write8(a, 0xC005, 0);
    write8(a, 0x200, 0x5A);
    assert(read8(a, 0x200) == 0xA5);
    write8(a, 0xC003, 0);
    assert(read8(a, 0x200) == 0x5A);

    // 80STORE + PAGE2 banks text and hi-res into the selected auxiliary bank.
    write8(a, 0xC002, 0); write8(a, 0xC004, 0);
    write8(a, 0xC001, 0); read8(a, 0xC055); read8(a, 0xC057);
    write8(a, 0x400, 0xA1); write8(a, 0x2000, 0xB1);
    assert(read8(a, 0x400) == 0xA1 && read8(a, 0x2000) == 0xB1);
    write8(a, 0xC073, 2);
    assert(read8(a, 0x400) == (2 ^ 4) && read8(a, 0x2000) == (2 ^ 5));
    write8(a, 0xC073, 1);
    assert(read8(a, 0x400) == 0xA1 && read8(a, 0x2000) == 0xB1);

    // Every double-width video mode keeps fetching bank 0 while CPU bank changes.
    memset(a->ram[1], 0xFF, 0x10000);
    memset(a->ramworks_extra, 0, 0x10000);
    for (size_t i = 0; i < sizeof a->char_rom; i++) a->char_rom[i] = (uint8_t)((i / 8) & 0x7F);
    uint32_t *screen = malloc(sizeof a->fb);
    assert(screen);
    a->col80 = a->dhires = true;
    for (int mode = 0; mode < 3; mode++) {
        a->text = mode == 0; a->hires = mode == 2;
        write8(a, 0xC073, 0); a2_render(a);
        memcpy(screen, a->fb, sizeof a->fb);
        write8(a, 0xC073, 1); a2_render(a);
        assert(memcmp(screen, a->fb, sizeof a->fb) == 0);
    }
    free(screen);

    write8(a, 0xC003, 0); write8(a, 0xC005, 0);
    write8(a, 0x200, 0x55);
    a2_reset(a);
    assert(a->ramworks_bank == 0);
    write8(a, 0xC003, 0); write8(a, 0xC073, 1);
    assert(read8(a, 0x200) == 0x55);
    a2_power_cycle(a);
    write8(a, 0xC003, 0); write8(a, 0xC073, 1);
    assert(read8(a, 0x200) == 0);
    assert(a2_set_ramworks(a, false));
    assert(!a2_ramworks(a) && a->ramworks_bank == 0);
    assert(a2_peek(a, 0x200) == a->ram[1][0x200]);
    a2_destroy(a);
}

static void address(apple2 *a, uint32_t addr)
{
    write8(a, 0xC090, (uint8_t)addr);
    write8(a, 0xC091, (uint8_t)(addr >> 8));
    write8(a, 0xC092, (uint8_t)(addr >> 16));
}

static void ramfactor(a2_model model)
{
    apple2 *a = a2_create(model);
    a2_power_cycle(a);
    assert(!a2_ramfactor(a));
    assert(!a2_set_ramfactor(a, true)); // Firmware required.
    uint8_t rom[0x2000];
    for (size_t i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i / 256);
    assert(!a2_load_ramfactor_rom(a, rom, sizeof rom - 1));
    assert(a2_load_ramfactor_rom(a, rom, sizeof rom));
    assert(a2_set_ramfactor(a, true));
    if (model != A2_MODEL_IIPLUS) {
        assert(a2_set_ramworks(a, true));
        write8(a, 0xC073, 7); // Both cards installed together.
    }

    // Distinct data in every 64 KB bank across the full 8 MB.
    for (int bank = 0; bank < 128; bank++) {
        address(a, (uint32_t)bank << 16);
        write8(a, 0xC093, (uint8_t)bank);
    }
    for (int bank = 0; bank < 128; bank++) {
        address(a, (uint32_t)bank << 16);
        assert(read8(a, 0xC093) == bank);
    }
    const uint32_t boundaries[] = {0xFF, 0xFFFF, 0x7FFFFF, 0xFFFFFF};
    for (size_t i = 0; i < sizeof boundaries / sizeof *boundaries; i++) {
        address(a, boundaries[i]);
        write8(a, 0xC093, 0xA5); write8(a, 0xC093, 0x5A);
        address(a, boundaries[i]);
        assert(read8(a, 0xC093) == 0xA5);
        uint32_t next = (boundaries[i] + 1) & 0xFFFFFF;
        assert(read8(a, 0xC090) == (next & 0xFF));
        assert(read8(a, 0xC091) == ((next >> 8) & 0xFF));
        assert(read8(a, 0xC092) == (next >> 16));
        assert(read8(a, 0xC093) == 0x5A);
    }
    // Partial address writes use the programmed address, not the auto-incremented one.
    address(a, 0x12FFFF); read8(a, 0xC093);
    write8(a, 0xC090, 0x20);
    assert(read8(a, 0xC091) == 0xFF && read8(a, 0xC092) == 0x12);

    assert(read8(a, 0xC100) == 1 && a->ramfactor_c800);
    assert(read8(a, 0xC800) == 8);
    write8(a, 0xC09F, 1);
    assert(read8(a, 0xC100) == 17);
    assert(read8(a, 0xC800) == 24);
    assert(read8(a, 0xCFFF) == 31 && !a->ramfactor_c800);
    write8(a, 0xC100, 0); // ROM writes also select expansion ROM.
    assert(a->ramfactor_c800);
    write8(a, 0xCFFF, 0);
    assert(!a->ramfactor_c800);

    if (model != A2_MODEL_IIPLUS) {
        memset(a->rom, 0xCC, sizeof a->rom);
        write8(a, 0xC007, 0); // INTCXROM overrides slot ROM without selecting the card.
        assert(read8(a, 0xC100) == 0xCC && !a->ramfactor_c800);
        write8(a, 0xC100, 0);
        assert(a->ramfactor_c800 && read8(a, 0xC800) == 0xCC);
        write8(a, 0xC006, 0);
        assert(read8(a, 0xC800) == 24);
        write8(a, 0xCFFF, 0);
        read8(a, 0xC300); // Internal 80-column ROM claims C800.
        assert(a->intc8rom);
        assert(read8(a, 0xC100) == 17);
        assert(read8(a, 0xC800) == 0xCC);
        read8(a, 0xCFFF);
        assert(!a->ramfactor_c800 && !a->intc8rom);
    }
    address(a, 0x1234); write8(a, 0xC093, 0x42);
    assert(a2_set_ramfactor(a, true)); // Reapplying configuration preserves memory.
    a2_reset(a);
    assert(read8(a, 0xC090) == 0 && read8(a, 0xC09F) == 0);
    address(a, 0x1234); assert(read8(a, 0xC093) == 0x42);
    a2_power_cycle(a);
    address(a, 0x1234); assert(read8(a, 0xC093) == 0xFF);
    assert(a2_set_ramfactor(a, false));
    assert(!a2_ramfactor(a) && !a->ramfactor_c800);
    a2_destroy(a);
}

int main(void)
{
    apple2 *plus = a2_create(A2_MODEL_IIPLUS);
    assert(!a2_set_ramworks(plus, true));
    a2_destroy(plus);
    ramworks(A2_MODEL_IIE);
    ramworks(A2_MODEL_IIE_ENHANCED);
    ramfactor(A2_MODEL_IIPLUS);
    ramfactor(A2_MODEL_IIE);
    ramfactor(A2_MODEL_IIE_ENHANCED);
    puts("RamWorks III and RamFactor bus tests PASS (II+, IIe, enhanced IIe)");
    return 0;
}
