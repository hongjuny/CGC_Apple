// SmartPort (slot 5) test against a fake FujiNet-PC.
//
// A thread connects to the card's TCP port and answers SP-over-SLIP requests
// for two devices: unit 1 is a 64-block disk whose block 0 is a boot program,
// unit 2 is a character device that echoes what is written to it. It also
// sends one stale reply first, which the card must ignore.
//
// The machine has no Disk II ROM, so the Autostart ROM boots slot 5. The boot
// program calls the ProDOS entry (READ, WRITE, STATUS) and the SmartPort entry
// (STATUS of the bus, READBLOCK, WRITE, READ, CONTROL, a bad unit), storing
// results in zero page for the checks below.
// Usage: sptest <romdir>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "a2internal.h"

enum { BLOCKS = 64, SIZE = BLOCKS * 512, PORT = 19885 };

// ---- fake FujiNet -----------------------------------------------------------

static uint8_t disk[SIZE];
static uint8_t echo[256];
static size_t echo_len;
static int inits, controls, last_control_code, last_control_len;
static uint8_t last_control_data[16];

static void send_frame(int fd, const uint8_t *p, size_t n)
{
    uint8_t out[2048];
    size_t k = 0;
    out[k++] = 0xC0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0xC0 || p[i] == 0xDB) {
            out[k++] = 0xDB;
            out[k++] = p[i] == 0xC0 ? 0xDC : 0xDD;
        } else {
            out[k++] = p[i];
        }
    }
    out[k++] = 0xC0;
    send(fd, out, k, 0);
}

static void reply(int fd, const uint8_t *req, size_t n)
{
    uint8_t resp[600] = { req[0], 0 };
    size_t len = 2;
    uint8_t cmd = req[1], unit = req[3];
    uint32_t block = req[6] | req[7] << 8 | req[8] << 16;
    switch (cmd) {
    case 0x05:                                  // INIT: two devices
        inits++;
        resp[1] = unit == 2 ? 0xFF : 0x00;
        break;
    case 0x00:                                  // STATUS: general status, then blocks
        resp[2] = unit == 1 ? 0xF8 : 0xE0;
        resp[3] = unit == 1 ? BLOCKS : 0;
        len = 6;
        break;
    case 0x01:                                  // READBLOCK
        memcpy(resp + 2, disk + block * 512, 512);
        len = 514;
        break;
    case 0x02:                                  // WRITEBLOCK
        memcpy(disk + block * 512, req + 11, 512);
        break;
    case 0x09:                                  // WRITE to the echo device
        echo_len = n - 11 < sizeof echo ? n - 11 : sizeof echo;
        memcpy(echo, req + 11, echo_len);
        break;
    case 0x08: {                                // READ from the echo device
        size_t want = (size_t)(req[6] | req[7] << 8);
        size_t got = want < echo_len ? want : echo_len;
        memcpy(resp + 2, echo, got);
        len = 2 + got;
        break;
    }
    case 0x04:                                  // CONTROL
        controls++;
        last_control_code = req[6];
        last_control_len = (int)(n - 11);
        memcpy(last_control_data, req + 11, n - 11 < 16 ? n - 11 : 16);
        break;
    default:
        resp[1] = 0x01;
        break;
    }
    send_frame(fd, resp, len);
}

static void *fujinet(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("connect");
        return NULL;
    }
    uint8_t stale[2] = { 0x77, 0x00 };          // a reply nobody asked for
    send_frame(fd, stale, sizeof stale);
    uint8_t buf[4096], frame[2048];
    size_t flen = 0;
    bool in = false, esc = false;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == 0xC0) {
                if (in && flen)
                    reply(fd, frame, flen);
                in = true;
                flen = 0;
                continue;
            }
            if (!in)
                continue;
            if (esc) {
                c = c == 0xDC ? 0xC0 : 0xDB;
                esc = false;
            } else if (c == 0xDB) {
                esc = true;
                continue;
            }
            if (flen < sizeof frame)
                frame[flen++] = c;
        }
    }
    close(fd);
    return NULL;
}

// ---- boot program -----------------------------------------------------------

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
static void set(uint8_t zp, uint8_t v) { emit(4, 0xA9, v, 0x85, zp); }
static void save_carry(uint8_t zp) { emit(5, 0xA9, 0x00, 0x2A, 0x85, zp); }
static void prodos(void) { emit(3, 0x20, 0x40, 0xC5); }
// JSR SmartPort entry ($C543) / DB cmd / DW list
static void sp_call(uint8_t cmd, uint16_t list) { emit(6, 0x20, 0x43, 0xC5, cmd, list & 0xFF, list >> 8); }

static void make_disk(void)
{
    for (int i = 0; i < SIZE; i++)
        disk[i] = (uint8_t)(i / 512 * 13 + i);  // every byte value, so SLIP escapes are exercised
    code = disk;
    pc = 0;
    emit(1, 0x01);
    emit(2, 0x86, 0x06);                        // STX $06 (slot * 16)
    // ProDOS entry
    set(0x42, 1); emit(4, 0xA5, 0x06, 0x85, 0x43);
    set(0x44, 0x00); set(0x45, 0x20); set(0x46, 1); set(0x47, 0);
    prodos(); save_carry(0x10);                 // READ block 1 -> $2000
    set(0x42, 2); set(0x45, 0x08); set(0x46, 3);
    prodos(); save_carry(0x11);                 // WRITE $0800 -> block 3
    set(0x42, 0);
    prodos(); emit(4, 0x86, 0x12, 0x84, 0x13);  // STATUS: X/Y = blocks
    // SmartPort entry; parameter lists live at $0980 (inside this block)
    sp_call(0x00, 0x0980); save_carry(0x14);  // STATUS unit 0 -> $3000
    sp_call(0x01, 0x0988); save_carry(0x15);  // READBLOCK unit 1, block 2 -> $2200
    sp_call(0x09, 0x0990); emit(2, 0x86, 0x16);   // WRITE 5 bytes to unit 2
    sp_call(0x08, 0x09A0); emit(2, 0x86, 0x17);   // READ 5 bytes from unit 2 -> $2400
    sp_call(0x04, 0x09B0); save_carry(0x18);  // CONTROL $10 to unit 2
    sp_call(0x00, 0x09B8); emit(2, 0x85, 0x19);   // STATUS unit 3: bad unit
    emit(3, 0x4C, (0x0800 + pc) & 0xFF, (0x0800 + pc) >> 8);

    static const uint8_t lists[][9] = {
        { 3, 0, 0x00, 0x30, 0x00 },                         // $0980 STATUS unit 0 code 0
        { 3, 1, 0x00, 0x22, 0x02, 0x00, 0x00 },             // $0988 READBLOCK
        { 4, 2, 0xC0, 0x09, 0x05, 0x00, 0, 0, 0 },          // $0990 WRITE from $09C0
        { 4, 2, 0x00, 0x24, 0x05, 0x00, 0, 0, 0 },          // $09A0 READ to $2400
        { 3, 2, 0xC8, 0x09, 0x10 },                         // $09B0 CONTROL list $09C8
        { 3, 3, 0x00, 0x30, 0x00 },                         // $09B8 STATUS unit 3
    };
    static const uint16_t at[] = { 0x180, 0x188, 0x190, 0x1A0, 0x1B0, 0x1B8 };
    for (int i = 0; i < 6; i++)
        memcpy(disk + at[i], lists[i], i == 0 || i == 4 || i == 5 ? 5 : i == 1 ? 7 : 9);
    memcpy(disk + 0x1C0, "HELLO", 5);                       // $09C0 data to write
    static const uint8_t ctl[] = { 2, 0, 0xC0, 0xDB };      // $09C8 control list
    memcpy(disk + 0x1C8, ctl, sizeof ctl);
}

static int check(const char *what, bool ok)
{
    printf("  %-42s %s\n", what, ok ? "PASS" : "FAIL");
    return !ok;
}

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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir\n", argv[0]);
        return 2;
    }
    make_disk();
    static uint8_t original[SIZE];
    memcpy(original, disk, SIZE);

    apple2 *a2 = a2_create(A2_MODEL_IIE_ENHANCED);
    if (!load(a2, argv[1], "Apple2e_Enhanced.rom", a2_load_system_rom) ||
        !load(a2, argv[1], "Apple2e_Enhanced_Video.rom", a2_load_char_rom)) {
        fprintf(stderr, "ROMs not found in %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    failures += check("listens on the port", a2_set_smartport(a2, true, PORT));
    failures += check("state: waiting", a2_smartport_state(a2) == 1);
    pthread_t t;
    pthread_create(&t, NULL, fujinet, NULL);
    usleep(200000);

    a2_power_cycle(a2);                         // no Disk II ROM: slot 5 boots
    for (int f = 0; f < 120; f++)
        a2_run_frame(a2);

    const uint8_t *zp = a2->ram[0];
    failures += check("connected, 2 units numbered by INIT", a2_smartport_state(a2) == 2 &&
                      a2_smartport_units(a2) == 2 && inits == 2);
    failures += check("booted slot 5 (X = $50)", zp[0x06] == 0x50);
    failures += check("ProDOS READ block 1", zp[0x10] == 0 &&
                      memcmp(a2->ram[0] + 0x2000, original + 512, 512) == 0);
    failures += check("ProDOS WRITE block 3", zp[0x11] == 0 && memcmp(disk + 3 * 512, original, 512) == 0);
    failures += check("ProDOS STATUS: 64 blocks", zp[0x12] == BLOCKS && zp[0x13] == 0);
    failures += check("SmartPort STATUS unit 0: 2 devices", zp[0x14] == 0 && a2->ram[0][0x3000] == 2);
    failures += check("SmartPort READBLOCK block 2", zp[0x15] == 0 &&
                      memcmp(a2->ram[0] + 0x2200, original + 1024, 512) == 0);
    failures += check("SmartPort WRITE: 5 bytes", zp[0x16] == 5 && echo_len == 5 && memcmp(echo, "HELLO", 5) == 0);
    failures += check("SmartPort READ: 5 bytes back", zp[0x17] == 5 && memcmp(a2->ram[0] + 0x2400, "HELLO", 5) == 0);
    static const uint8_t ctl[] = { 2, 0, 0xC0, 0xDB };
    failures += check("SmartPort CONTROL with its list", zp[0x18] == 0 && last_control_code == 0x10 &&
                      last_control_len == 4 && memcmp(last_control_data, ctl, 4) == 0);
    failures += check("SmartPort bad unit: $11", zp[0x19] == 0x11);

    int before = controls;
    a2_reset(a2);
    failures += check("Ctrl-RESET sends CONTROL $00 to each unit", controls == before + 2 && last_control_code == 0);

    a2_destroy(a2);                             // closes the connection; the thread ends
    pthread_join(t, NULL);
    printf(failures ? "FAIL\n" : "all PASS\n");
    return failures ? 1 : 0;
}
