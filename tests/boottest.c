// Headless boot test: powers on the machine, optionally boots a disk and types
// keystrokes, then prints the 40x24 text screen and writes a PPM screenshot.
// Usage: boottest <romdir> [disk.dsk] [frames] [keys] [out.ppm]
//   keys: typed after boot; '\n' in the string becomes RETURN.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "a2core.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static bool load_rom(apple2 *a2, const char *dir, const char *name,
                     bool (*loader)(apple2 *, const uint8_t *, size_t))
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    uint8_t *data = slurp(path, &len);
    bool ok = data && loader(a2, data, len);
    free(data);
    printf("%-18s %s\n", name, ok ? "loaded" : "MISSING");
    return ok;
}

static void dump_text(apple2 *a2)
{
    for (int row = 0; row < 24; row++) {
        uint16_t base = (uint16_t)(0x400 + ((row & 7) << 7) + (row >> 3) * 40);
        char line[41];
        for (int col = 0; col < 40; col++) {
            uint8_t c = a2_peek(a2, (uint16_t)(base + col)) & 0x7F;
            if (c < 0x20)
                c += 0x40;  // inverse/flash glyphs map to @A-Z[\]^_
            else if (c >= 0x60 && !(a2_peek(a2, (uint16_t)(base + col)) & 0x80))
                c -= 0x20;  // II+ flashing space and friends
            line[col] = (char)c;
        }
        line[40] = 0;
        printf("|%s|\n", line);
    }
}

static void write_ppm(apple2 *a2, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", A2_SCREEN_W, A2_SCREEN_H);
    const uint32_t *fb = a2_framebuffer(a2);
    for (int i = 0; i < A2_SCREEN_W * A2_SCREEN_H; i++) {
        uint8_t rgb[3] = { fb[i] & 0xFF, (fb[i] >> 8) & 0xFF, (fb[i] >> 16) & 0xFF };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(apple2 *a2, int frames)
{
    int16_t sink[4096];
    while (frames-- > 0) {
        a2_run_frame(a2);
        a2_take_audio(a2, sink, sizeof sink / sizeof sink[0] / 2);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir [disk.dsk] [frames] [keys] [out.ppm]\n", argv[0]);
        return 2;
    }
    // A2MODEL=iie or iiee selects the Apple IIe / enhanced IIe (default II+).
    const char *model_name = getenv("A2MODEL") ? getenv("A2MODEL") : "ii+";
    a2_model model = A2_MODEL_IIPLUS;
    const char *sys_rom = "Apple2_Plus.rom", *char_rom = "Apple2_Video.rom";
    if (strcmp(model_name, "iie") == 0) {
        model = A2_MODEL_IIE;
        sys_rom = "Apple2e.rom";
        char_rom = "Apple2e_Enhanced_Video.rom";
    } else if (strcmp(model_name, "iiee") == 0) {
        model = A2_MODEL_IIE_ENHANCED;
        sys_rom = "Apple2e_Enhanced.rom";
        char_rom = "Apple2e_Enhanced_Video.rom";
    }
    apple2 *a2 = a2_create(model);
    // PADDLES="x,y" sets paddles 0 and 1 (default 127,127).
    if (getenv("PADDLES")) {
        int px = 127, py = 127;
        sscanf(getenv("PADDLES"), "%d,%d", &px, &py);
        a2_set_paddle(a2, 0, (uint8_t)px);
        a2_set_paddle(a2, 1, (uint8_t)py);
    }
    if (getenv("MBCOUNT"))   // Mockingboards installed (default 1)
        a2_set_mockingboards(a2, atoi(getenv("MBCOUNT")));
    if (!load_rom(a2, argv[1], sys_rom, a2_load_system_rom) ||
        !load_rom(a2, argv[1], char_rom, a2_load_char_rom))
        return 1;
    if (!getenv("NODISKROM"))   // NODISKROM=1: no boot PROM, so the II+ drops into BASIC
        load_rom(a2, argv[1], "DISK2.rom", a2_load_disk_rom);

    if (argc > 2 && argv[2][0]) {
        size_t len;
        uint8_t *img = slurp(argv[2], &len);
        const char *ext = strrchr(argv[2], '.');
        a2_disk_format fmt = A2_DISK_DOS_ORDER;
        if (ext && strcasecmp(ext, ".woz") == 0) fmt = A2_DISK_WOZ;
        else if (ext && strcasecmp(ext, ".po") == 0) fmt = A2_DISK_PRODOS_ORDER;
        else if (ext && strcasecmp(ext, ".nib") == 0) fmt = A2_DISK_NIBBLE;
        bool ok = img && a2_disk_insert(a2, 0, img, len, fmt, false);
        printf("disk %s: %s\n", argv[2], ok ? "inserted" : "FAILED");
        free(img);
    }
    int frames = argc > 3 ? atoi(argv[3]) : 120;

    a2_power_cycle(a2);
    run(a2, frames);

    if (argc > 4) {
        for (const char *k = argv[4]; *k; k++) {
            char c = *k;
            if (c == '\\' && k[1] == 'n') { c = '\r'; k++; }
            else if (c == '\n') c = '\r';
            a2_key_press(a2, (uint8_t)c);
            // Wait for the ROM to consume the key, then give it a moment.
            for (int i = 0; i < 600 && a2_key_pending(a2); i++)
                run(a2, 1);
            run(a2, getenv("KEYGAP") ? atoi(getenv("KEYGAP")) : 2);
        }
        run(a2, frames);
    }

    printf("PC=$%04X\n", a2_cpu_pc(a2));
    dump_text(a2);
    if (argc > 5 && argv[5][0])
        write_ppm(a2, argv[5]);
    // SAVEDISK=path writes drive 1 back out in its original format.
    if (getenv("SAVEDISK") && a2_disk_present(a2, 0)) {
        size_t size = a2_disk_export_size(a2, 0);
        uint8_t *buf = malloc(size);
        size_t n = buf ? a2_disk_export(a2, 0, buf, size) : 0;
        FILE *f = n ? fopen(getenv("SAVEDISK"), "wb") : NULL;
        if (f) {
            fwrite(buf, 1, n, f);
            fclose(f);
        }
        printf("saved %zu bytes to %s (dirty=%d)\n", n, getenv("SAVEDISK"), a2_disk_dirty(a2, 0));
        free(buf);
    }
    a2_destroy(a2);
    return 0;
}
