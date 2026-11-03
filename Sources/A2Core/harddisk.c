// ProDOS block device in slot 7: an emulator-specific card with its own
// small firmware (no ROM dump needed). The firmware identifies itself to the
// Autostart ROM and ProDOS, and its driver entry reads one I/O register;
// that read performs the ProDOS command found in zero page ($42-$47) at once,
// moving 512-byte blocks straight between the image and Apple II memory.
//
// Images: raw ProDOS-order blocks (.hdv, .po) or 2IMG (.2mg, ProDOS order).
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

enum {
    HD_SLOT = 7,
    HD_MAX_BLOCKS = 65535,          // ProDOS volume limit (32 MB)
    TWOIMG_HEADER = 64,
};

enum {                              // ProDOS driver error codes
    ERR_NONE = 0x00,
    ERR_IO = 0x27,
    ERR_NO_DEVICE = 0x28,
    ERR_WRITE_PROTECTED = 0x2B,
};

// ---- firmware ($C700-$C7FF) -------------------------------------------------

#define DRIVER 0x40                 // offset of the ProDOS driver entry

static void build_rom(uint8_t *rom)
{
    static const uint8_t boot[] = {
        // Signature read by the Autostart ROM and ProDOS ($Cn01=$20, $Cn03=$00,
        // $Cn05=$03, $Cn07=$3C), written as harmless code like the Disk II's.
        0xA2, 0x20,                 // LDX #$20
        0xA0, 0x00,                 // LDY #$00
        0xA2, 0x03,                 // LDX #$03
        0x86, 0x3C,                 // STX $3C
        // Boot: read block 0 to $0800 and run it with X = slot * 16.
        0xA9, 0x01, 0x85, 0x42,     // LDA #1 : STA $42   (READ)
        0xA9, 0x70, 0x85, 0x43,     // LDA #$70 : STA $43 (slot 7, drive 1)
        0xA9, 0x00, 0x85, 0x44,     // LDA #0 : STA $44   (buffer $0800)
        0x85, 0x46, 0x85, 0x47,     // STA $46 : STA $47  (block 0)
        0xA9, 0x08, 0x85, 0x45,     // LDA #8 : STA $45
        0x20, DRIVER, 0xC7,         // JSR driver
        0xB0, 0x05,                 // BCS fail
        0xA2, 0x70,                 // LDX #$70
        0x4C, 0x01, 0x08,           // JMP $0801
        0x4C, 0x00, 0xC6,           // fail: JMP $C600 (boot the Disk II)
    };
    static const uint8_t driver[] = {
        0xAD, 0xF0, 0xC0,           // LDA $C0F0  (do the command; A = error)
        0xAE, 0xF1, 0xC0,           // LDX $C0F1  (block count, low)
        0xAC, 0xF2, 0xC0,           // LDY $C0F2  (block count, high)
        0xC9, 0x01,                 // CMP #1     (carry set on error)
        0x60,                       // RTS
    };
    memset(rom, 0, 256);
    memcpy(rom, boot, sizeof boot);
    memcpy(rom + DRIVER, driver, sizeof driver);
    rom[0xFC] = 0x00;               // block count 0: ask STATUS
    rom[0xFD] = 0x00;
    rom[0xFE] = 0x07;               // status, read and write supported; 1 volume
    rom[0xFF] = DRIVER;             // driver entry = $C700 + $40
}

// ---- image ------------------------------------------------------------------

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool a2_hd_insert(apple2 *a2, const uint8_t *data, size_t len, bool write_protected)
{
    size_t off = 0, size = len;
    if (len >= TWOIMG_HEADER && memcmp(data, "2IMG", 4) == 0) {
        uint32_t format = rd32le(data + 0x0C), flags = rd32le(data + 0x10);
        off = rd32le(data + 0x18);
        size = rd32le(data + 0x1C);
        if (format != 1 || off < TWOIMG_HEADER || off > len || size > len - off)
            return false;           // only ProDOS-order 2IMG images are block devices
        if (flags & 0x80000000u)
            write_protected = true;
    }
    if (size == 0 || size % 512 != 0 || size / 512 > HD_MAX_BLOCKS)
        return false;
    uint8_t *file = malloc(len);
    if (!file)
        return false;
    memcpy(file, data, len);
    a2_hd_eject(a2);
    harddisk *hd = &a2->hd;
    hd->file = file;
    hd->file_len = len;
    hd->data_off = off;
    hd->blocks = (uint32_t)(size / 512);
    hd->write_protected = write_protected;
    hd->dirty = false;
    build_rom(hd->rom);
    return true;
}

void a2_hd_eject(apple2 *a2)
{
    harddisk *hd = &a2->hd;
    free(hd->file);
    memset(hd, 0, sizeof *hd);
}

bool a2_hd_present(const apple2 *a2) { return a2->hd.file != NULL; }
bool a2_hd_dirty(const apple2 *a2) { return a2->hd.file && a2->hd.dirty; }
void a2_hd_mark_clean(apple2 *a2) { a2->hd.dirty = false; }
size_t a2_hd_export_size(const apple2 *a2) { return a2->hd.file_len; }

bool a2_hd_active(const apple2 *a2)
{
    return a2->hd.file && a2->hd.accessed && a2->frame_count - a2->hd.last_access_frame < 6;
}

size_t a2_hd_export(apple2 *a2, uint8_t *out, size_t max)
{
    const harddisk *hd = &a2->hd;
    if (!hd->file || max < hd->file_len)
        return 0;
    memcpy(out, hd->file, hd->file_len);
    return hd->file_len;
}

// ---- card -------------------------------------------------------------------

uint8_t hd_rom_read(const apple2 *a2, uint16_t addr)
{
    return a2->hd.rom[addr & 0xFF];
}

static uint8_t hd_command(apple2 *a2)
{
    harddisk *hd = &a2->hd;
    uint8_t cmd = bus_read(a2, 0x42), unit = bus_read(a2, 0x43);
    uint16_t buf = (uint16_t)(bus_read(a2, 0x44) | bus_read(a2, 0x45) << 8);
    uint32_t block = (uint32_t)(bus_read(a2, 0x46) | bus_read(a2, 0x47) << 8);
    hd->accessed = true;
    hd->last_access_frame = a2->frame_count;
    if ((unit & 0x80) || ((unit >> 4) & 7) != HD_SLOT)
        return ERR_NO_DEVICE;       // drive 2, or a unit mapped from another slot
    switch (cmd) {
    case 0:                         // STATUS
        return hd->write_protected ? ERR_WRITE_PROTECTED : ERR_NONE;
    case 1:                         // READ
        if (block >= hd->blocks)
            return ERR_IO;
        for (int i = 0; i < 512; i++)
            bus_write(a2, (uint16_t)(buf + i), hd->file[hd->data_off + block * 512 + i]);
        return ERR_NONE;
    case 2:                         // WRITE
        if (block >= hd->blocks)
            return ERR_IO;
        if (hd->write_protected)
            return ERR_WRITE_PROTECTED;
        for (int i = 0; i < 512; i++)
            hd->file[hd->data_off + block * 512 + i] = bus_read(a2, (uint16_t)(buf + i));
        hd->dirty = true;
        return ERR_NONE;
    case 3:                         // FORMAT: nothing to lay down
        return hd->write_protected ? ERR_WRITE_PROTECTED : ERR_NONE;
    default:
        return ERR_IO;
    }
}

uint8_t hd_io(apple2 *a2, uint8_t reg, bool write)
{
    harddisk *hd = &a2->hd;
    if (write)
        return 0;
    switch (reg) {
    case 0: return hd_command(a2);
    case 1: return (uint8_t)hd->blocks;
    case 2: return (uint8_t)(hd->blocks >> 8);
    default: return 0;
    }
}
