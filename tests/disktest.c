// Disk image round trips through the bit-level disk engine:
//   .dsk -> bits -> .dsk in DOS and ProDOS sector order
//   .dsk -> WOZ 2 -> (reload) -> .dsk
//   .dsk -> .nib -> (reload) -> .dsk
// Usage: disktest <image.dsk> [out.woz]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2core.h"

static uint8_t *export_as(apple2 *a2, a2_disk_format fmt, size_t *len)
{
    size_t size = a2_disk_export_size_as(a2, 0, fmt);
    uint8_t *buf = malloc(size);
    *len = buf ? a2_disk_export_as(a2, 0, fmt, buf, size) : 0;
    return buf;
}

static bool same_as(const uint8_t *img, const uint8_t *out, size_t n)
{
    return n == A2_DSK_SIZE && memcmp(img, out, n) == 0;
}

static int check(const char *name, bool ok)
{
    printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Converts a DOS-order image to `fmt`, reloads it and converts back.
static bool via(const uint8_t *img, a2_disk_format fmt, const char *save_path)
{
    apple2 *a = a2_create(A2_MODEL_IIPLUS);
    a2_disk_insert(a, 0, img, A2_DSK_SIZE, A2_DISK_DOS_ORDER, false);
    size_t len;
    uint8_t *mid = export_as(a, fmt, &len);
    a2_destroy(a);
    if (save_path && len) {
        FILE *f = fopen(save_path, "wb");
        if (f) { fwrite(mid, 1, len, f); fclose(f); }
    }

    apple2 *b = a2_create(A2_MODEL_IIPLUS);
    bool ok = len && a2_disk_insert(b, 0, mid, len, fmt, false);
    size_t n = 0;
    uint8_t *back = ok ? export_as(b, A2_DISK_DOS_ORDER, &n) : NULL;
    ok = ok && same_as(img, back, n);
    free(mid);
    free(back);
    a2_destroy(b);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s image.dsk [out.woz]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t img[A2_DSK_SIZE];
    size_t len = fread(img, 1, sizeof img, f);
    fclose(f);

    int failures = 0;
    a2_disk_format orders[2] = { A2_DISK_DOS_ORDER, A2_DISK_PRODOS_ORDER };
    for (int i = 0; i < 2; i++) {
        apple2 *a2 = a2_create(A2_MODEL_IIPLUS);
        bool ok = a2_disk_insert(a2, 0, img, len, orders[i], false);
        size_t n = 0;
        uint8_t *out = ok ? export_as(a2, orders[i], &n) : NULL;
        failures += check(i ? "ProDOS order round trip" : "DOS order round trip", ok && same_as(img, out, n));
        free(out);
        a2_destroy(a2);
    }
    failures += check("dsk -> WOZ2 -> dsk", via(img, A2_DISK_WOZ, argc > 2 ? argv[2] : NULL));
    failures += check("dsk -> nib -> dsk", via(img, A2_DISK_NIBBLE, NULL));
    return failures ? 1 : 0;
}
