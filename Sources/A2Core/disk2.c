// Disk II controller (slot 6), emulated at the bit level so that WOZ images
// (including copy-protected disks) work. Every image becomes a set of
// bit-stream tracks behind a quarter-track map; .dsk/.po/.nib images are
// converted to GCR bit streams on insert and decoded back when saved.
//
// The head passes one bit cell every 4 CPU cycles. The read path is a
// simplified logic state sequencer: bits shift into the data register until
// bit 7 is set; a complete byte then ignores further 0 bits (which is what
// makes 10-bit sync bytes self-synchronising) and stays visible for two bit
// cells after it completes. Long runs of 0 bits read as random noise, like
// the MC3470 amplifier on a real drive (WOZ reference, "fake bits").
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

#define CYCLES_PER_BIT          4
#define EMPTY_TRACK_BITS        51200
#define HOLD_BIT_CELLS          2
// The spindle keeps turning for about a second after the motor is switched
// off; DOS relies on this to skip its spin-up delay between calls.
#define MOTOR_SPINDOWN_CYCLES   1020484

static const uint8_t gcr62[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

// Physical sector -> sector index within the image file.
static const uint8_t dos_order[16]    = { 0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15 };
static const uint8_t prodos_order[16] = { 0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15 };

#define DISK_VOLUME 254

static const uint8_t *sector_map(a2_disk_format fmt)
{
    return fmt == A2_DISK_PRODOS_ORDER ? prodos_order : dos_order;
}

// ---- track storage ---------------------------------------------------------

static void drive_clear(disk_drive *dr)
{
    for (int i = 0; i < DISK_QTRACKS; i++) {
        free(dr->bits[i]);
        dr->bits[i] = NULL;
        dr->bit_count[i] = 0;
    }
    free(dr->orig);
    dr->orig = NULL;
    dr->orig_len = 0;
    dr->track_count = 0;
    memset(dr->tmap, 0xFF, sizeof dr->tmap);
    memset(dr->woz_info, 0, sizeof dr->woz_info);
    dr->present = dr->dirty = false;
    dr->bit_pos = 0;
}

static int current_track(const disk_drive *dr)
{
    int q = dr->halftrack * 2;
    int t = q < DISK_QTRACKS ? dr->tmap[q] : 0xFF;
    return (t != 0xFF && dr->bits[t]) ? t : -1;
}

static uint32_t track_length(const disk_drive *dr, int t)
{
    return t >= 0 ? dr->bit_count[t] : EMPTY_TRACK_BITS;
}

// Standard quarter-track map for 35-track images: each track also answers
// one quarter step either side of it, half-tracks are empty.
static void standard_tmap(disk_drive *dr)
{
    for (int q = 0; q < DISK_QTRACKS; q++) {
        int t = (q + 1) / 4;
        dr->tmap[q] = ((q & 3) != 2 && t < DISK_TRACKS) ? (uint8_t)t : 0xFF;
    }
}

// ---- encoding ---------------------------------------------------------------

typedef struct { uint8_t *buf; uint32_t bit; } bitwriter;

static void put_bit(bitwriter *w, int b)
{
    if (b)
        w->buf[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
    w->bit++;
}
static void put_byte(bitwriter *w, uint8_t v) { for (int i = 7; i >= 0; i--) put_bit(w, (v >> i) & 1); }
static void put44(bitwriter *w, uint8_t v) { put_byte(w, (v >> 1) | 0xAA); put_byte(w, v | 0xAA); }
static void put_sync(bitwriter *w, int n)   // 10-bit self-sync bytes
{
    while (n--) {
        put_byte(w, 0xFF);
        put_bit(w, 0);
        put_bit(w, 0);
    }
}

static void encode_sector(bitwriter *w, int track, int sector, const uint8_t *data)
{
    put_byte(w, 0xD5); put_byte(w, 0xAA); put_byte(w, 0x96);
    put44(w, DISK_VOLUME);
    put44(w, (uint8_t)track);
    put44(w, (uint8_t)sector);
    put44(w, (uint8_t)(DISK_VOLUME ^ track ^ sector));
    put_byte(w, 0xDE); put_byte(w, 0xAA); put_byte(w, 0xEB);
    put_sync(w, 6);

    // 6-and-2 prenibblize: 86 bytes of packed low bit pairs, then 256 high six-bit values.
    uint8_t nibbles[342] = {0};
    int aux = 0x55;
    for (int i = 0x101; i >= 0; i--) {
        uint8_t v6 = data[i & 0xFF];
        uint8_t v2 = nibbles[aux];
        v2 = (uint8_t)((v2 << 1) | (v6 & 1)); v6 >>= 1;
        v2 = (uint8_t)((v2 << 1) | (v6 & 1)); v6 >>= 1;
        nibbles[aux] = v2;
        if (i < 0x100)
            nibbles[0x56 + i] = v6;
        if (--aux < 0)
            aux = 0x55;
    }

    put_byte(w, 0xD5); put_byte(w, 0xAA); put_byte(w, 0xAD);
    uint8_t last = 0;
    for (int i = 0; i < 342; i++) {
        put_byte(w, gcr62[(last ^ nibbles[i]) & 0x3F]);
        last = nibbles[i];
    }
    put_byte(w, gcr62[last & 0x3F]);
    put_byte(w, 0xDE); put_byte(w, 0xAA); put_byte(w, 0xEB);
    put_sync(w, 16);
}

static bool alloc_track(disk_drive *dr, int t, uint32_t max_bits)
{
    dr->bits[t] = calloc((max_bits + 7) / 8, 1);
    return dr->bits[t] != NULL;
}

static bool load_sector_image(disk_drive *dr, const uint8_t *data, a2_disk_format fmt)
{
    const uint8_t *map = sector_map(fmt);
    for (int t = 0; t < DISK_TRACKS; t++) {
        if (!alloc_track(dr, t, DISK_TRACK_LEN * 8))
            return false;
        bitwriter w = { dr->bits[t], 0 };
        put_sync(&w, 64);
        for (int s = 0; s < 16; s++)
            encode_sector(&w, t, s, data + t * 4096 + map[s] * 256);
        dr->bit_count[t] = w.bit;
    }
    dr->track_count = DISK_TRACKS;
    standard_tmap(dr);
    dr->orig = malloc(A2_DSK_SIZE);
    if (!dr->orig)
        return false;
    memcpy(dr->orig, data, A2_DSK_SIZE);
    dr->orig_len = A2_DSK_SIZE;
    return true;
}

static bool load_nibble_image(disk_drive *dr, const uint8_t *data)
{
    for (int t = 0; t < DISK_TRACKS; t++) {
        if (!alloc_track(dr, t, DISK_TRACK_LEN * 8))
            return false;
        bitwriter w = { dr->bits[t], 0 };
        for (int i = 0; i < DISK_TRACK_LEN; i++)
            put_byte(&w, data[t * DISK_TRACK_LEN + i]);
        dr->bit_count[t] = w.bit;
    }
    dr->track_count = DISK_TRACKS;
    standard_tmap(dr);
    return true;
}

// ---- WOZ images ---------------------------------------------------------------

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static bool load_woz(disk_drive *dr, const uint8_t *data, size_t len)
{
    static const uint8_t tail[4] = { 0xFF, 0x0A, 0x0D, 0x0A };
    if (len < 12 || memcmp(data, "WOZ", 3) != 0 || (data[3] != '1' && data[3] != '2') ||
        memcmp(data + 4, tail, 4) != 0)
        return false;
    int version = data[3] - '0';
    uint32_t crc = rd32le(data + 8);
    if (crc != 0 && crc != crc32(data + 12, len - 12))
        return false;

    bool have_info = false, have_tmap = false, have_trks = false;
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *id = data + off;
        uint32_t size = rd32le(data + off + 4);
        const uint8_t *body = data + off + 8;
        if (size > len - off - 8)
            break;
        if (memcmp(id, "INFO", 4) == 0 && size >= 37) {
            memcpy(dr->woz_info, body, size < 60 ? size : 60);
            if (body[1] != 1)
                return false;               // 3.5" disks are not supported
            have_info = true;
        } else if (memcmp(id, "TMAP", 4) == 0 && size >= DISK_QTRACKS) {
            memcpy(dr->tmap, body, DISK_QTRACKS);
            have_tmap = true;
        } else if (memcmp(id, "TRKS", 4) == 0) {
            if (version == 1) {
                // WOZ1: fixed 6656-byte records (6646 bytes of bits, then bytes used and bit count).
                int n = (int)(size / 6656);
                for (int t = 0; t < n && t < DISK_QTRACKS; t++) {
                    const uint8_t *rec = body + t * 6656;
                    uint32_t bits = rd16le(rec + 6648);
                    if (bits == 0 || bits > 6646 * 8 || !alloc_track(dr, t, bits))
                        continue;
                    memcpy(dr->bits[t], rec, (bits + 7) / 8);
                    dr->bit_count[t] = bits;
                    dr->track_count = t + 1;
                }
            } else {
                // WOZ2: 160 TRK entries (start block, block count, bit count); data at 512-byte blocks.
                if (size < DISK_QTRACKS * 8)
                    return false;
                for (int t = 0; t < DISK_QTRACKS; t++) {
                    const uint8_t *e = body + t * 8;
                    uint32_t start = rd16le(e), bits = rd32le(e + 4);
                    size_t bytes = (bits + 7) / 8;
                    if (start == 0 || bits == 0 || (size_t)start * 512 + bytes > len || !alloc_track(dr, t, bits))
                        continue;
                    memcpy(dr->bits[t], data + (size_t)start * 512, bytes);
                    dr->bit_count[t] = bits;
                    dr->track_count = t + 1;
                }
            }
            have_trks = true;
        }
        off += 8 + size;
    }
    if (!have_info || !have_tmap || !have_trks)
        return false;
    for (int q = 0; q < DISK_QTRACKS; q++)
        if (dr->tmap[q] != 0xFF && (dr->tmap[q] >= DISK_QTRACKS || !dr->bits[dr->tmap[q]]))
            dr->tmap[q] = 0xFF;
    return true;
}

static size_t woz_size(const disk_drive *dr)
{
    size_t blocks = 0;
    for (int t = 0; t < DISK_QTRACKS; t++)
        if (dr->bits[t])
            blocks += ((dr->bit_count[t] + 7) / 8 + 511) / 512;
    return 1536 + blocks * 512;   // header, INFO, TMAP and the TRK table fill blocks 0-2
}

static size_t save_woz(const disk_drive *dr, uint8_t *out, size_t max)
{
    size_t total = woz_size(dr);
    if (max < total)
        return 0;
    memset(out, 0, total);
    memcpy(out, "WOZ2\xFF\x0A\x0D\x0A", 8);

    uint8_t *info = out + 12;
    memcpy(info, "INFO", 4);
    wr32le(info + 4, 60);
    uint8_t *ib = info + 8;
    ib[0] = 2;                                           // INFO version
    ib[1] = 1;                                           // 5.25" disk
    ib[2] = dr->woz_info[2];                             // write protected
    ib[3] = dr->woz_info[3];                             // synchronized
    ib[4] = dr->woz_info[4];                             // cleaned
    memset(ib + 5, ' ', 32);
    memcpy(ib + 5, "CGC Apple", 9);
    ib[37] = 1;                                          // disk sides
    ib[38] = dr->woz_info[0] >= 2 ? dr->woz_info[38] : 0;  // boot sector format
    ib[39] = 32;                                         // optimal bit timing (4 us)
    if (dr->woz_info[0] >= 2) {
        memcpy(ib + 40, dr->woz_info + 40, 4);           // compatible hardware, required RAM
    }

    uint8_t *tmap = info + 68;
    memcpy(tmap, "TMAP", 4);
    wr32le(tmap + 4, DISK_QTRACKS);
    memcpy(tmap + 8, dr->tmap, DISK_QTRACKS);

    uint8_t *trks = tmap + 8 + DISK_QTRACKS;
    memcpy(trks, "TRKS", 4);
    wr32le(trks + 4, (uint32_t)(total - 1536 + DISK_QTRACKS * 8));
    uint16_t block = 3, largest = 0;
    for (int t = 0; t < DISK_QTRACKS; t++) {
        if (!dr->bits[t])
            continue;
        size_t bytes = (dr->bit_count[t] + 7) / 8;
        uint16_t nblocks = (uint16_t)((bytes + 511) / 512);
        uint8_t *e = trks + 8 + t * 8;
        wr16le(e, block);
        wr16le(e + 2, nblocks);
        wr32le(e + 4, dr->bit_count[t]);
        memcpy(out + (size_t)block * 512, dr->bits[t], bytes);
        block = (uint16_t)(block + nblocks);
        if (nblocks > largest)
            largest = nblocks;
    }
    wr16le(ib + 44, largest);                            // largest track, in blocks
    wr32le(out + 8, crc32(out + 12, total - 12));
    return total;
}

// ---- decoding (saving back to sector and nibble images) ---------------------

// Runs the read logic over a track (twice round, so sectors across the
// splice point are seen whole) and returns the byte stream a program would read.
static size_t track_nibbles(const disk_drive *dr, int t, uint8_t *out, size_t max)
{
    uint32_t n = dr->bit_count[t];
    uint8_t shift = 0;
    size_t count = 0;
    for (uint32_t i = 0; i < 2 * n && count < max; i++) {
        uint32_t p = i % n;
        int b = (dr->bits[t][p >> 3] >> (7 - (p & 7))) & 1;
        if (shift & 0x80) {
            if (b)
                shift = 1;
        } else {
            shift = (uint8_t)((shift << 1) | b);
            if (shift & 0x80)
                out[count++] = shift;
        }
    }
    return count;
}

static int ungcr(uint8_t v)
{
    for (int i = 0; i < 64; i++)
        if (gcr62[i] == v)
            return i;
    return -1;
}

static bool decode_sectors(const uint8_t *t, size_t n, a2_disk_format fmt, uint8_t *out /* 4096 */)
{
    const uint8_t *map = sector_map(fmt);
    bool found[16] = {false};

    for (size_t i = 0; i + 3 + 8 < n; i++) {
        if (t[i] != 0xD5 || t[i + 1] != 0xAA || t[i + 2] != 0x96)
            continue;
        size_t p = i + 3;
        uint8_t sec = (uint8_t)(((t[p + 4] << 1) | 1) & t[p + 5]);
        if (sec > 15 || found[sec])
            continue;
        // Data prologue follows within a short gap.
        size_t q = p + 8, limit = q + 64;
        while (q + 3 < n && q < limit && !(t[q] == 0xD5 && t[q + 1] == 0xAA && t[q + 2] == 0xAD))
            q++;
        if (q >= limit || q + 3 + 343 > n)
            continue;
        q += 3;

        uint8_t nibbles[342];
        uint8_t last = 0;
        bool ok = true;
        for (int k = 0; k < 342; k++) {
            int v = ungcr(t[q + k]);
            if (v < 0) { ok = false; break; }
            last ^= (uint8_t)v;
            nibbles[k] = last;
        }
        int chk = ok ? ungcr(t[q + 342]) : -1;
        if (chk < 0 || (uint8_t)chk != last)
            continue;

        uint8_t *dst = out + map[sec] * 256;
        for (int k = 0; k < 256; k++) {
            uint8_t low = (uint8_t)(nibbles[k % 86] >> ((k / 86) * 2)) & 3;
            low = (uint8_t)(((low & 1) << 1) | (low >> 1));
            dst[k] = (uint8_t)((nibbles[0x56 + k] << 2) | low);
        }
        found[sec] = true;
    }
    for (int s = 0; s < 16; s++)
        if (!found[s])
            return false;
    return true;
}

static size_t save_sectors(const disk_drive *dr, a2_disk_format fmt, uint8_t *out, size_t max)
{
    if (max < A2_DSK_SIZE)
        return 0;
    // Start from the original image so tracks that no longer decode keep their data.
    if (dr->orig && dr->orig_len == A2_DSK_SIZE)
        memcpy(out, dr->orig, A2_DSK_SIZE);
    else
        memset(out, 0, A2_DSK_SIZE);
    static uint8_t nib[2 * 8192];
    for (int t = 0; t < DISK_TRACKS; t++) {
        int idx = dr->tmap[t * 4];
        if (idx == 0xFF || !dr->bits[idx])
            continue;
        size_t n = track_nibbles(dr, idx, nib, sizeof nib);
        uint8_t buf[4096];
        if (decode_sectors(nib, n, fmt, buf))
            memcpy(out + t * 4096, buf, sizeof buf);
    }
    return A2_DSK_SIZE;
}

static size_t save_nibbles(const disk_drive *dr, uint8_t *out, size_t max)
{
    if (max < A2_NIB_SIZE)
        return 0;
    static uint8_t nib[2 * 8192];
    for (int t = 0; t < DISK_TRACKS; t++) {
        uint8_t *dst = out + t * DISK_TRACK_LEN;
        memset(dst, 0xFF, DISK_TRACK_LEN);
        int idx = dr->tmap[t * 4];
        if (idx == 0xFF || !dr->bits[idx])
            continue;
        size_t n = track_nibbles(dr, idx, nib, sizeof nib);
        // The second revolution is in sync; take one track's worth from there.
        size_t start = n / 2;
        for (size_t i = 0; i < DISK_TRACK_LEN && start + i < n; i++)
            dst[i] = nib[start + i];
    }
    return A2_NIB_SIZE;
}

// ---- controller ------------------------------------------------------------

bool disk2_spinning(const disk2 *d, uint64_t cycle)
{
    return d->motor || cycle - d->motor_off_cycle < MOTOR_SPINDOWN_CYCLES;
}

void disk2_power_on(disk2 *d)
{
    d->motor = false;
    d->motor_off_cycle = (uint64_t)0 - MOTOR_SPINDOWN_CYCLES;   // already stopped at cycle 0
    d->bit_clock = 0;
    d->shift = d->held = 0;
    d->hold = 0;
    d->rng = 0x12345678u;
    disk2_reset(d);
}

void disk2_reset(disk2 *d)
{
    if (d->motor)
        d->motor_off_cycle = d->now;
    d->motor = false;
    d->q6 = d->q7 = false;
    d->phases = 0;
    d->active = 0;
}

bool disk2_insert(disk2 *d, int drive, const uint8_t *data, size_t len, a2_disk_format fmt, bool wp)
{
    if (drive != 0 && drive != 1)
        return false;
    disk_drive *dr = &d->drive[drive];
    drive_clear(dr);
    bool ok;
    switch (fmt) {
    case A2_DISK_WOZ:
        ok = load_woz(dr, data, len);
        if (ok && dr->woz_info[2])
            wp = true;
        break;
    case A2_DISK_NIBBLE:
        ok = len == A2_NIB_SIZE && load_nibble_image(dr, data);
        break;
    default:
        ok = len == A2_DSK_SIZE && load_sector_image(dr, data, fmt);
        break;
    }
    if (!ok) {
        drive_clear(dr);
        return false;
    }
    dr->format = fmt;
    dr->present = true;
    dr->write_protected = wp;
    return true;
}

void disk2_eject(disk2 *d, int drive)
{
    if (drive == 0 || drive == 1)
        drive_clear(&d->drive[drive]);
}

size_t disk2_export_size(const disk2 *d, int drive, a2_disk_format fmt)
{
    if ((drive != 0 && drive != 1) || !d->drive[drive].present)
        return 0;
    switch (fmt) {
    case A2_DISK_WOZ: return woz_size(&d->drive[drive]);
    case A2_DISK_NIBBLE: return A2_NIB_SIZE;
    default: return A2_DSK_SIZE;
    }
}

size_t disk2_export(disk2 *d, int drive, a2_disk_format fmt, uint8_t *out, size_t max)
{
    if ((drive != 0 && drive != 1) || !d->drive[drive].present)
        return 0;
    const disk_drive *dr = &d->drive[drive];
    switch (fmt) {
    case A2_DISK_WOZ: return save_woz(dr, out, max);
    case A2_DISK_NIBBLE: return save_nibbles(dr, out, max);
    default: return save_sectors(dr, fmt, out, max);
    }
}

static int next_bit(disk2 *d, disk_drive *dr)
{
    int t = current_track(dr);
    uint32_t len = track_length(dr, t);
    int raw = 0;
    if (t >= 0)
        raw = (dr->bits[t][dr->bit_pos >> 3] >> (7 - (dr->bit_pos & 7))) & 1;
    dr->bit_pos = (dr->bit_pos + 1) % len;

    // MC3470: with no flux transitions for a while, the amplifier's gain
    // rises until noise reads as random 1 bits.
    d->zero_window = (uint8_t)(((d->zero_window << 1) | raw) & 0x0F);
    if (d->zero_window == 0) {
        d->rng ^= d->rng << 13;
        d->rng ^= d->rng >> 17;
        d->rng ^= d->rng << 5;
        return (d->rng % 10) < 3;
    }
    return raw;
}

static void write_bit(disk2 *d, disk_drive *dr)
{
    int t = current_track(dr);
    uint32_t len = track_length(dr, t);
    int b = d->write_bits_out < 8 ? (d->write_byte >> (7 - d->write_bits_out)) & 1 : 0;
    d->write_bits_out++;
    if (t >= 0 && !dr->write_protected) {
        uint8_t mask = (uint8_t)(0x80 >> (dr->bit_pos & 7));
        if (b)
            dr->bits[t][dr->bit_pos >> 3] |= mask;
        else
            dr->bits[t][dr->bit_pos >> 3] &= (uint8_t)~mask;
        dr->dirty = true;
    }
    dr->bit_pos = (dr->bit_pos + 1) % len;
}

static void read_cell(disk2 *d, disk_drive *dr)
{
    int b = next_bit(d, dr);
    bool was_full = d->shift & 0x80;
    if (was_full) {
        if (b)
            d->shift = 1;          // next byte starts; 0 bits after a full byte are ignored
    } else {
        d->shift = (uint8_t)((d->shift << 1) | b);
    }
    if (!was_full && (d->shift & 0x80)) {
        d->held = d->shift;
        d->hold = HOLD_BIT_CELLS;
    } else if (d->hold > 0) {
        d->hold--;
    }
}

// Brings the head and the data register up to `cycle`.
static void advance(disk2 *d, uint64_t cycle)
{
    uint64_t target = cycle / CYCLES_PER_BIT;
    uint64_t end = target;
    if (!d->motor) {
        uint64_t stop = (d->motor_off_cycle + MOTOR_SPINDOWN_CYCLES) / CYCLES_PER_BIT;
        if (end > stop)
            end = stop;
    }
    disk_drive *dr = &d->drive[d->active];
    if (end > d->bit_clock && dr->present) {
        uint64_t n = end - d->bit_clock;
        uint32_t len = track_length(dr, current_track(dr));
        // Only the last few cells matter for the data register when reading;
        // skip the rest of a long idle stretch.
        if (!d->q7 && n > 64) {
            dr->bit_pos = (uint32_t)((dr->bit_pos + (n - 64)) % len);
            n = 64;
        }
        while (n--) {
            if (d->q7)
                write_bit(d, dr);
            else
                read_cell(d, dr);
        }
    }
    d->bit_clock = target;
}

static void step_motor(disk2 *d, int phase, bool on)
{
    if (on)
        d->phases |= (uint8_t)(1 << phase);
    else
        d->phases &= (uint8_t)~(1 << phase);
    if (!on)
        return;

    disk_drive *dr = &d->drive[d->active];
    int cur = dr->halftrack & 3;
    int delta = 0;
    if (phase == ((cur + 1) & 3))
        delta = 1;
    else if (phase == ((cur + 3) & 3))
        delta = -1;
    int ht = dr->halftrack + delta;
    if (ht < 0)
        ht = 0;
    if (ht > DISK_QTRACKS / 2 - 1)
        ht = DISK_QTRACKS / 2 - 1;
    if (ht == dr->halftrack)
        return;

    // Keep the angular position when moving between tracks of different lengths.
    uint32_t old_len = track_length(dr, current_track(dr));
    dr->halftrack = ht;
    uint32_t new_len = track_length(dr, current_track(dr));
    dr->bit_pos = (uint32_t)((uint64_t)dr->bit_pos * new_len / old_len) % new_len;
}

uint8_t disk2_io(disk2 *d, uint8_t reg, bool write, uint8_t value, uint64_t cycle)
{
    advance(d, cycle);
    d->now = cycle;
    switch (reg) {
    case 0x0: case 0x1: case 0x2: case 0x3:
    case 0x4: case 0x5: case 0x6: case 0x7:
        step_motor(d, reg >> 1, reg & 1);
        break;
    case 0x8:
        if (d->motor)
            d->motor_off_cycle = cycle;
        d->motor = false;
        break;
    case 0x9: d->motor = true; break;
    case 0xA: d->active = 0; break;
    case 0xB: d->active = 1; break;
    case 0xC: d->q6 = false; break;
    case 0xD: d->q6 = true; break;
    case 0xE: d->q7 = false; break;
    case 0xF:
        if (!d->q7)
            d->write_bits_out = 8;  // nothing loaded yet: write zeros
        d->q7 = true;
        break;
    }

    disk_drive *dr = &d->drive[d->active];
    if (d->q7) {
        if (d->q6 && write) {
            d->write_byte = value;  // write load
            d->write_bits_out = 0;
        }
        return 0;
    }
    if (reg & 1)
        return 0;
    if (d->q6)
        return dr->write_protected ? 0x80 : 0x00;   // write-protect sense
    return d->hold > 0 ? d->held : d->shift;
}
