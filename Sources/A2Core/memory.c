// Applied Engineering RamFactor register and ROM behaviour, independently
// implemented using MAME's bus/a2bus/a2memexp.cpp as a hardware reference
// (BSD-3-Clause, R. Belmont). RamWorks MMU integration is in apple2.c.
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

bool a2_load_ramfactor_rom(apple2 *a2, const uint8_t *data, size_t len)
{
    if (!data || len != sizeof a2->ramfactor_rom)
        return false;
    memcpy(a2->ramfactor_rom, data, len);
    a2->ramfactor_rom_loaded = true;
    return true;
}

void ramfactor_reset(apple2 *a2)
{
    memset(a2->ramfactor_regs, 0, sizeof a2->ramfactor_regs);
    a2->ramfactor_address = a2->ramfactor_cursor = 0;
    a2->ramfactor_c800 = false;
}

bool a2_set_ramfactor(apple2 *a2, bool enabled)
{
    if (enabled == (a2->ramfactor_ram != NULL))
        return true;
    if (enabled) {
        if (a2->midi_slot == 1 || !a2->ramfactor_rom_loaded)
            return false;
        uint8_t *ram = malloc(EXPANSION_RAM_SIZE);
        if (!ram)
            return false;
        memset(ram, 0xFF, EXPANSION_RAM_SIZE);
        a2->ramfactor_ram = ram;
    } else {
        free(a2->ramfactor_ram);
        a2->ramfactor_ram = NULL;
    }
    ramfactor_reset(a2);
    return true;
}

bool a2_ramfactor(const apple2 *a2)
{
    return a2->ramfactor_ram != NULL;
}

uint8_t ramfactor_io(apple2 *a2, uint8_t reg, bool write, uint8_t value)
{
    uint8_t *regs = a2->ramfactor_regs;
    if (write && reg < 3) {
        // Partial writes rebuild the last programmed address, not the live cursor.
        unsigned shift = reg * 8;
        a2->ramfactor_address = (a2->ramfactor_address & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        a2->ramfactor_cursor = a2->ramfactor_address;
    } else if (reg == 3) {
        uint32_t index = a2->ramfactor_cursor & (EXPANSION_RAM_SIZE - 1);
        if (write)
            a2->ramfactor_ram[index] = value;
        else
            value = a2->ramfactor_ram[index];
        a2->ramfactor_cursor = (a2->ramfactor_cursor + 1) & 0xFFFFFF;
    } else {
        if (write)
            regs[reg] = value;
        return regs[reg];
    }
    for (int i = 0; i < 3; i++)
        regs[i] = (uint8_t)(a2->ramfactor_cursor >> (i * 8));
    return write ? 0 : value;
}

uint8_t ramfactor_rom_read(const apple2 *a2, uint16_t addr)
{
    unsigned bank = (a2->ramfactor_regs[15] & 1) * 0x1000;
    unsigned offset = addr < 0xC800 ? 0x100 + (addr & 0xFF) : 0x800 + (addr & 0x7FF);
    return a2->ramfactor_rom[bank + offset];
}
