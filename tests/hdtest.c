// Slot 7 hard disk test. Builds a 64-block image whose block 0 is a boot
// program, powers on with the real ROMs (the Autostart ROM must boot slot 7
// before the Disk II), and checks the driver from inside the Apple II:
// STATUS (block count), READ, WRITE, an out-of-range READ, then the same
// image as 2IMG and write-protected.
// Usage: hdtest <romdir>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

enum { BLOCKS = 64, SIZE = BLOCKS * 512 };

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
static void set(uint8_t zp, uint8_t value) { emit(4, 0xA9, value, 0x85, zp); }   // LDA #v : STA zp
static void call(void) { emit(3, 0x20, 0x40, 0xC7); }                             // JSR $C740
static void save_carry(uint8_t zp) { emit(5, 0xA9, 0x00, 0x2A, 0x85, zp); }       // LDA #0 : ROL : STA zp

static bool load(apple2 *a2, const char *dir, const char *name,
                 bool (*loader)(apple2 *, const uint8_t *, size_t))
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

static void make_image(uint8_t *img)
{
    for (int i = 0; i < SIZE; i++)
        img[i] = (uint8_t)(i / 512 * 7 + i);       // distinct pattern per block
    code = img;
    pc = 0;
    emit(1, 0x01);                  // ProDOS boot blocks start with $01
    emit(2, 0x86, 0x06);            // STX $06 (slot * 16 from the firmware)
    set(0x42, 1); emit(4, 0xA5, 0x06, 0x85, 0x43);   // READ, unit = X
    set(0x44, 0x00); set(0x45, 0x20); set(0x46, 1); set(0x47, 0);
    call(); save_carry(0x10);       // block 1 -> $2000
    set(0x42, 2); set(0x44, 0x00); set(0x45, 0x08); set(0x46, 2);
    call(); save_carry(0x11);       // $0800 (this block) -> block 2
    set(0x42, 0);
    call(); emit(4, 0x86, 0x12, 0x84, 0x13); save_carry(0x14);   // STATUS: X/Y = blocks
    set(0x42, 1); set(0x46, 100);
    call(); emit(2, 0x85, 0x15); save_carry(0x16);   // block 100: error $27
    set(0x42, 1); set(0x43, 0xF0); set(0x46, 1);
    call(); emit(2, 0x85, 0x17);    // drive 2: error $28
    emit(3, 0x4C, (0x0800 + pc) & 0xFF, (0x0800 + pc) >> 8);   // JMP * (block 0 runs at $0800)
}

static int check(const char *what, bool ok)
{
    printf("  %-38s %s\n", what, ok ? "PASS" : "FAIL");
    return !ok;
}

static int run(const char *romdir, a2_model model, const uint8_t *image, size_t len,
               size_t data_off, bool wp)
{
    static const char *names[][2] = {
        { "Apple2_Plus.rom", "Apple2_Video.rom" },
        { "Apple2e.rom", "Apple2e_Enhanced_Video.rom" },
        { "Apple2e_Enhanced.rom", "Apple2e_Enhanced_Video.rom" },
    };
    apple2 *a2 = a2_create(model);
    if (!load(a2, romdir, names[model][0], a2_load_system_rom) ||
        !load(a2, romdir, names[model][1], a2_load_char_rom)) {
        fprintf(stderr, "ROMs not found in %s\n", romdir);
        exit(2);
    }
    load(a2, romdir, "DISK2.rom", a2_load_disk_rom);
    if (!a2_hd_insert(a2, image, len, wp)) {
        printf("  insert FAILED\n");
        return 1;
    }
    a2_power_cycle(a2);
    for (int f = 0; f < 120; f++)
        a2_run_frame(a2);

    const uint8_t *zp = a2->ram[0];
    const uint8_t *blocks = image + data_off;
    static uint8_t out[SIZE + 64];
    size_t n = a2_hd_export(a2, out, sizeof out);
    int failures = 0;
    failures += check("booted slot 7 (X = $70)", zp[0x06] == 0x70);
    failures += check("READ block 1", zp[0x10] == 0 && memcmp(a2->ram[0] + 0x2000, blocks + 512, 512) == 0);
    failures += check("STATUS: 64 blocks", zp[0x12] == BLOCKS && zp[0x13] == 0 &&
                      zp[0x14] == (wp ? 1 : 0));
    failures += check("out-of-range READ: $27, carry", zp[0x15] == 0x27 && zp[0x16] == 1);
    failures += check("drive 2: $28", zp[0x17] == 0x28);
    if (wp) {
        failures += check("WRITE refused, image unchanged", zp[0x11] == 1 && !a2_hd_dirty(a2) &&
                          n == len && memcmp(out, image, len) == 0);
    } else {
        bool written = memcmp(out + data_off + 1024, blocks, 512) == 0;
        bool header = memcmp(out, image, data_off) == 0;
        failures += check("WRITE block 2, export keeps header", zp[0x11] == 0 && a2_hd_dirty(a2) &&
                          n == len && written && header);
    }
    a2_destroy(a2);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir\n", argv[0]);
        return 2;
    }
    static uint8_t raw[SIZE], twoimg[64 + SIZE];
    make_image(raw);
    memcpy(twoimg, "2IMG", 4);
    memcpy(twoimg + 4, "CGCA", 4);
    twoimg[0x08] = 64;              // header size
    twoimg[0x0C] = 1;               // ProDOS order
    twoimg[0x14] = BLOCKS;          // block count
    twoimg[0x18] = 64;              // data offset
    twoimg[0x1C] = SIZE & 0xFF; twoimg[0x1D] = (SIZE >> 8) & 0xFF;
    memcpy(twoimg + 64, raw, SIZE);

    int failures = 0;
    const char *model_names[] = { "II+", "IIe", "enhanced IIe" };
    for (int m = 0; m < 3; m++) {
        printf("%s, .hdv\n", model_names[m]);
        failures += run(argv[1], (a2_model)m, raw, SIZE, 0, false);
    }
    printf("enhanced IIe, .2mg\n");
    failures += run(argv[1], A2_MODEL_IIE_ENHANCED, twoimg, sizeof twoimg, 64, false);
    printf("II+, write-protected\n");
    failures += run(argv[1], A2_MODEL_IIPLUS, raw, SIZE, 0, true);

    apple2 *a2 = a2_create(A2_MODEL_IIPLUS);
    failures += check("rejects odd-sized image", !a2_hd_insert(a2, raw, 1000, false));
    twoimg[0x0C] = 0;               // DOS-order 2IMG is a floppy, not a block device
    failures += check("rejects DOS-order 2IMG", !a2_hd_insert(a2, twoimg, sizeof twoimg, false));
    a2_hd_insert(a2, raw, SIZE, false);
    a2_power_cycle(a2);
    a2->ram[0][0x43] = 0x70;
    a2->ram[0][0x46] = a2->ram[0][0x47] = 0;
    const uint16_t invalid[] = { 0xBE01, 0xC000, 0xC0F0, 0xCFFF, 0xFE01, 0xFFFF };
    bool rejected = true;
    for (int cmd = 1; cmd <= 2; cmd++) {
        a2->ram[0][0x42] = (uint8_t)cmd;
        for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
            a2->ram[0][0x44] = (uint8_t)invalid[i];
            a2->ram[0][0x45] = (uint8_t)(invalid[i] >> 8);
            rejected &= bus_read(a2, 0xC0F0) == 0x27;
        }
    }
    failures += check("rejects I/O and wrapped buffers", rejected && !a2_hd_dirty(a2) &&
                      memcmp(a2->hd.file, raw, SIZE) == 0);
    const uint16_t valid[] = { 0xBE00, 0xD000, 0xFE00 };
    bool accepted = true;
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        a2->ram[0][0x42] = 1;
        a2->ram[0][0x44] = (uint8_t)valid[i];
        a2->ram[0][0x45] = (uint8_t)(valid[i] >> 8);
        accepted &= bus_read(a2, 0xC0F0) == 0;
    }
    failures += check("accepts RAM boundary buffers", accepted);
    a2_destroy(a2);
    printf(failures ? "FAIL\n" : "all PASS\n");
    return failures ? 1 : 0;
}
