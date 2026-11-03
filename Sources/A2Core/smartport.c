// SmartPort card in slot 5 that relays to FujiNet-PC ("SP over SLIP").
//
// The card has its own small firmware (no ROM dump needed) with the standard
// ProDOS block entry and SmartPort entry. Each entry reads one I/O register,
// and that read performs the whole call: the emulator decodes the request,
// sends it to FujiNet-PC as a SLIP-framed packet over TCP and waits for the
// reply, as a real SmartPort call also waits for the bus. FujiNet-PC connects
// to us (default port 1985).
//
// Packet layout follows FujiNet's lib/devrelay (GPL-3.0, read as a protocol
// reference only): request = [seq, command, param count, unit, 0, 0,
// 5 parameter bytes, data...]; response = [seq, status, data...]. Units are
// numbered by INIT, one at a time, until a device answers $FF (end of chain).
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "a2internal.h"

enum {
    SP_SLOT = 5,
    SP_MAX_UNITS = 15,
    SP_TIMEOUT_MS = 30000,          // long TNFS transfers take seconds
    SP_INIT_TIMEOUT_MS = 2000,
    SP_RESET_TIMEOUT_MS = 500,
    SP_RX_CAP = 1 << 18,
    SP_RESP_CAP = 0x10000 + 16,
};

enum { SLIP_END = 0xC0, SLIP_ESC = 0xDB, SLIP_ESC_END = 0xDC, SLIP_ESC_ESC = 0xDD };

enum {                              // SmartPort commands
    CMD_STATUS, CMD_READBLOCK, CMD_WRITEBLOCK, CMD_FORMAT, CMD_CONTROL,
    CMD_INIT, CMD_OPEN, CMD_CLOSE, CMD_READ, CMD_WRITE,
};

static const uint8_t param_counts[] = { 3, 3, 3, 1, 3, 1, 1, 1, 4, 4 };

enum {                              // SmartPort and ProDOS error codes
    ERR_NONE = 0x00, ERR_BADCMD = 0x01, ERR_BADPCNT = 0x04, ERR_BUSERR = 0x06,
    ERR_BADUNIT = 0x11, ERR_BADCTL = 0x21, ERR_IO = 0x27, ERR_NO_DEVICE = 0x28,
    ERR_END_OF_CHAIN = 0xFF,
};

// ---- firmware ($C500-$C5FF) -------------------------------------------------

#define DRIVER 0x40                 // ProDOS entry; the SmartPort entry is DRIVER + 3

static void build_rom(uint8_t *rom)
{
    static const uint8_t boot[] = {
        // Signature: $Cn01=$20, $Cn03=$00, $Cn05=$03 and $Cn07=$00 (SmartPort).
        0xA2, 0x20,                 // LDX #$20
        0xA0, 0x00,                 // LDY #$00
        0xA2, 0x03,                 // LDX #$03
        0xA2, 0x00,                 // LDX #$00
        // Boot: read block 0 of unit 1 to $0800 and run it with X = slot * 16.
        0xA9, 0x01, 0x85, 0x42,     // LDA #1 : STA $42   (READ)
        0xA9, 0x50, 0x85, 0x43,     // LDA #$50 : STA $43 (slot 5, drive 1)
        0xA9, 0x00, 0x85, 0x44,     // LDA #0 : STA $44   (buffer $0800)
        0x85, 0x46, 0x85, 0x47,     // STA $46 : STA $47  (block 0)
        0xA9, 0x08, 0x85, 0x45,     // LDA #8 : STA $45
        0x20, DRIVER, 0xC5,         // JSR ProDOS entry
        0xB0, 0x05,                 // BCS fail
        0xA2, 0x50,                 // LDX #$50
        0x4C, 0x01, 0x08,           // JMP $0801
        0x4C, 0x00, 0xC6,           // fail: JMP $C600 (boot the Disk II)
    };
    static const uint8_t driver[] = {
        0x18,                       // +0  ProDOS entry: CLC
        0x90, 0x06,                 // +1  BCC +9
        0xAD, 0xD3, 0xC0,           // +3  SmartPort entry: LDA $C0D3 (the call; A = error)
        0x4C, DRIVER + 12, 0xC5,    // +6  JMP +12
        0xAD, 0xD0, 0xC0,           // +9  LDA $C0D0 (ProDOS block command; A = error)
        0xAE, 0xD1, 0xC0,           // +12 LDX $C0D1 (count or blocks, low)
        0xAC, 0xD2, 0xC0,           // +15 LDY $C0D2 (high)
        0xC9, 0x01,                 // +18 CMP #1 (carry set on error)
        0x60,                       // +20 RTS
    };
    memset(rom, 0, 256);
    memcpy(rom, boot, sizeof boot);
    memcpy(rom + DRIVER, driver, sizeof driver);
    rom[0xFB] = 0x00;               // SmartPort ID: no extended calls
    rom[0xFC] = 0x00;               // block count 0: ask STATUS
    rom[0xFD] = 0x00;
    rom[0xFE] = 0x97;               // removable, 2 volumes, read/write/status
    rom[0xFF] = DRIVER;
}

// ---- transport --------------------------------------------------------------

static void close_conn(smartport *sp)
{
    if (sp->conn_fd >= 0)
        close(sp->conn_fd);
    sp->conn_fd = -1;
    sp->units = -1;
    sp->rx_len = 0;
}

static bool send_all(smartport *sp, const uint8_t *p, size_t n)
{
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(sp->conn_fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(sp->conn_fd, p, n, 0);
#endif
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// Takes the next complete SLIP frame out of the receive buffer, decoded.
static int next_frame(smartport *sp, uint8_t *out, size_t max)
{
    for (;;) {
        size_t a = 0;
        while (a < sp->rx_len && sp->rx[a] != SLIP_END)
            a++;
        if (a == sp->rx_len) {
            sp->rx_len = 0;         // noise before any frame
            return -1;
        }
        size_t b = a + 1;
        while (b < sp->rx_len && sp->rx[b] != SLIP_END)
            b++;
        if (b == sp->rx_len) {      // frame not complete yet: keep it
            memmove(sp->rx, sp->rx + a, sp->rx_len - a);
            sp->rx_len -= a;
            return -1;
        }
        size_t n = 0;
        bool bad = false;
        for (size_t i = a + 1; i < b; i++) {
            uint8_t c = sp->rx[i];
            if (c == SLIP_ESC && i + 1 < b)
                c = sp->rx[++i] == SLIP_ESC_END ? SLIP_END : SLIP_ESC;
            if (n < max)
                out[n++] = c;
            else
                bad = true;
        }
        memmove(sp->rx, sp->rx + b, sp->rx_len - b);   // the closing END may open the next frame
        sp->rx_len -= b;
        if (n > 0 && !bad)
            return (int)n;
    }
}

static bool receive(smartport *sp, int timeout_ms)
{
    struct pollfd p = { sp->conn_fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0)
        return r == 0 || errno == EINTR;
    if (sp->rx_len == SP_RX_CAP)
        sp->rx_len = 0;             // runaway stream: drop it
    ssize_t n = recv(sp->conn_fd, sp->rx + sp->rx_len, SP_RX_CAP - sp->rx_len, 0);
    if (n <= 0) {
        close_conn(sp);
        return false;
    }
    sp->rx_len += (size_t)n;
    return true;
}

// Sends one request and waits for the response with the same sequence number.
// Returns the response length, or -1 on timeout or a lost connection.
static int transact(smartport *sp, uint8_t *req, size_t len, uint8_t *resp, size_t max, int timeout_ms)
{
    if (sp->conn_fd < 0)
        return -1;
    req[0] = ++sp->seq;
    uint8_t *enc = malloc(len * 2 + 2);
    if (!enc)
        return -1;
    size_t n = 0;
    enc[n++] = SLIP_END;
    for (size_t i = 0; i < len; i++) {
        if (req[i] == SLIP_END || req[i] == SLIP_ESC) {
            enc[n++] = SLIP_ESC;
            enc[n++] = req[i] == SLIP_END ? SLIP_ESC_END : SLIP_ESC_ESC;
        } else {
            enc[n++] = req[i];
        }
    }
    enc[n++] = SLIP_END;
    bool sent = send_all(sp, enc, n);
    free(enc);
    if (!sent) {
        close_conn(sp);
        return -1;
    }
    for (int waited = 0; waited < timeout_ms && sp->conn_fd >= 0; waited += 10) {
        int got;
        while ((got = next_frame(sp, resp, max)) >= 0) {
            if (got >= 2 && resp[0] == req[0])
                return got;         // anything else is a stale reply from before a reset
        }
        if (!receive(sp, 10))
            return -1;
    }
    return -1;
}

static void request_header(uint8_t *req, uint8_t cmd, uint8_t unit)
{
    memset(req, 0, 11);
    req[1] = cmd;
    req[2] = param_counts[cmd];
    req[3] = unit;
}

// Numbers the devices on a new connection: INIT units 1, 2, ... until $FF.
static void enumerate(smartport *sp)
{
    sp->units = 0;
    for (int id = 1; id <= SP_MAX_UNITS && sp->conn_fd >= 0; id++) {
        uint8_t req[11], resp[16];
        request_header(req, CMD_INIT, (uint8_t)id);
        int n = transact(sp, req, sizeof req, resp, sizeof resp, SP_INIT_TIMEOUT_MS);
        if (n < 2)
            break;
        sp->units = id;
        if (resp[1] == ERR_END_OF_CHAIN)
            break;
    }
}

static void accept_pending(smartport *sp)
{
    if (sp->listen_fd < 0 || sp->conn_fd >= 0)
        return;
    int fd = accept(sp->listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    sp->conn_fd = fd;
    sp->units = -1;
    sp->rx_len = 0;
}

// Accepts FujiNet-PC, numbers its devices and notices when it goes away.
void sp_poll(apple2 *a2)
{
    smartport *sp = &a2->sp;
    if (!sp->enabled)
        return;
    accept_pending(sp);
    if (sp->conn_fd < 0)
        return;
    if (sp->units < 0)
        enumerate(sp);
    struct pollfd p = { sp->conn_fd, POLLIN, 0 };
    if (sp->conn_fd >= 0 && poll(&p, 1, 0) > 0)
        receive(sp, 0);             // unsolicited bytes, or the peer closing
}

// After Ctrl-RESET each device gets CONTROL $00, so a modem can hang up.
void sp_reset(apple2 *a2)
{
    smartport *sp = &a2->sp;
    for (int id = 1; sp->conn_fd >= 0 && id <= sp->units; id++) {
        uint8_t req[13] = { 0 }, resp[16];
        request_header(req, CMD_CONTROL, (uint8_t)id);
        transact(sp, req, sizeof req, resp, sizeof resp, SP_RESET_TIMEOUT_MS);   // empty control list
    }
}

// ---- card -------------------------------------------------------------------

static void copy_in(apple2 *a2, uint8_t *dst, uint16_t src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = bus_read(a2, (uint16_t)(src + i));
}

static void copy_out(apple2 *a2, uint16_t dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++)
        bus_write(a2, (uint16_t)(dst + i), src[i]);
}

static uint16_t read16(apple2 *a2, uint16_t addr)
{
    return (uint16_t)(bus_read(a2, addr) | bus_read(a2, (uint16_t)(addr + 1)) << 8);
}

static int units(apple2 *a2)
{
    sp_poll(a2);
    return a2->sp.units > 0 ? a2->sp.units : 0;
}

// Relays one device call. `out_len` bytes from Apple II memory at `buf` follow
// the header; returned data goes back to `buf`. *moved = bytes transferred.
static uint8_t device_call(apple2 *a2, uint8_t cmd, uint8_t unit, uint16_t buf,
                           const uint8_t params[5], size_t out_len, size_t *moved)
{
    smartport *sp = &a2->sp;
    *moved = 0;
    uint8_t *req = malloc(11 + out_len);
    if (!req)
        return ERR_IO;
    request_header(req, cmd, unit);
    memcpy(req + 6, params, 5);
    copy_in(a2, req + 11, buf, out_len);
    sp->accessed = true;
    sp->last_access_frame = a2->frame_count;
    int n = transact(sp, req, 11 + out_len, sp->resp, SP_RESP_CAP, SP_TIMEOUT_MS);
    free(req);
    if (n < 2)
        return ERR_BUSERR;
    uint8_t status = sp->resp[1];
    if (status == ERR_NONE && (cmd == CMD_STATUS || cmd == CMD_READBLOCK || cmd == CMD_READ)) {
        copy_out(a2, buf, sp->resp + 2, (size_t)n - 2);
        *moved = (size_t)n - 2;
    } else if (status == ERR_NONE && (cmd == CMD_WRITE || cmd == CMD_WRITEBLOCK)) {
        *moved = out_len;
    }
    return status;
}

// ProDOS block driver: command in $42, unit in $43, buffer $44-$45, block $46-$47.
static uint8_t prodos_call(apple2 *a2)
{
    smartport *sp = &a2->sp;
    uint8_t cmd = bus_read(a2, 0x42), unit = bus_read(a2, 0x43);
    uint16_t buf = read16(a2, 0x44);
    uint8_t params[5] = { bus_read(a2, 0x46), bus_read(a2, 0x47) };
    int drive = (unit & 0x80) ? 2 : 1;
    sp->x_out = sp->y_out = 0;
    if (cmd > CMD_FORMAT)
        return ERR_IO;
    if (((unit >> 4) & 7) != SP_SLOT || drive > units(a2))
        return ERR_NO_DEVICE;
    size_t moved;
    uint8_t err;
    if (cmd == CMD_STATUS) {
        // Device status (code 0): 4 bytes, the block count in bytes 1-3.
        uint8_t req[11];
        request_header(req, CMD_STATUS, (uint8_t)drive);
        sp->accessed = true;
        sp->last_access_frame = a2->frame_count;
        int n = transact(sp, req, sizeof req, sp->resp, SP_RESP_CAP, SP_TIMEOUT_MS);
        if (n < 2)
            return ERR_IO;
        if (sp->resp[1] != ERR_NONE)
            return sp->resp[1];
        if (n >= 5) {
            sp->x_out = sp->resp[3];
            sp->y_out = sp->resp[4];
        }
        return ERR_NONE;
    }
    size_t out = cmd == CMD_WRITEBLOCK ? 512 : 0;
    err = device_call(a2, cmd, (uint8_t)drive, buf, params, out, &moved);
    return err == ERR_BUSERR ? ERR_IO : err;
}

// SmartPort entry: JSR entry / DB command / DW parameter list. The return
// address on the stack is moved past the inline bytes.
static uint8_t smartport_call(apple2 *a2)
{
    smartport *sp = &a2->sp;
    uint16_t s = (uint16_t)(0x100 | ((a2->cpu.sp + 1) & 0xFF));
    uint16_t s2 = (uint16_t)(0x100 | ((a2->cpu.sp + 2) & 0xFF));
    uint16_t ret = (uint16_t)(bus_read(a2, s) | bus_read(a2, s2) << 8);
    uint8_t cmd = bus_read(a2, (uint16_t)(ret + 1));
    bool extended = cmd & 0x40;
    uint16_t list = read16(a2, (uint16_t)(ret + 2));
    uint16_t next = (uint16_t)(ret + (extended ? 5 : 3));
    bus_write(a2, s, (uint8_t)next);
    bus_write(a2, s2, (uint8_t)(next >> 8));
    sp->x_out = sp->y_out = 0;

    if (extended || cmd > CMD_WRITE)
        return ERR_BADCMD;
    if (bus_read(a2, list) != param_counts[cmd])
        return ERR_BADPCNT;
    uint8_t unit = bus_read(a2, (uint16_t)(list + 1));
    uint16_t buf = read16(a2, (uint16_t)(list + 2));
    int count = units(a2);

    if (unit == 0) {                    // the SmartPort itself
        if (cmd == CMD_STATUS && bus_read(a2, (uint16_t)(list + 4)) == 0) {
            uint8_t status[8] = { (uint8_t)count };
            copy_out(a2, buf, status, sizeof status);
            sp->x_out = sizeof status;
            return ERR_NONE;
        }
        return cmd == CMD_INIT ? ERR_NONE : cmd == CMD_STATUS ? ERR_BADCTL : ERR_BADUNIT;
    }
    if (unit > count)
        return ERR_BADUNIT;

    uint8_t params[5] = { 0 };
    size_t out = 0, moved = 0;
    switch (cmd) {
    case CMD_STATUS:
    case CMD_CONTROL:
        params[0] = bus_read(a2, (uint16_t)(list + 4));         // status or control code
        if (cmd == CMD_CONTROL)
            out = (size_t)read16(a2, buf) + 2;                  // control list with its length word
        break;
    case CMD_READBLOCK:
    case CMD_WRITEBLOCK:
        for (int i = 0; i < 3; i++)
            params[i] = bus_read(a2, (uint16_t)(list + 4 + i)); // block number
        out = cmd == CMD_WRITEBLOCK ? 512 : 0;
        break;
    case CMD_READ:
    case CMD_WRITE:
        for (int i = 0; i < 5; i++)
            params[i] = bus_read(a2, (uint16_t)(list + 4 + i)); // byte count, address
        out = cmd == CMD_WRITE ? (size_t)(params[0] | params[1] << 8) : 0;
        break;
    default:
        break;
    }
    uint8_t err = device_call(a2, cmd, unit, buf, params, out, &moved);
    sp->x_out = (uint8_t)moved;
    sp->y_out = (uint8_t)(moved >> 8);
    return err;
}

uint8_t sp_rom_read(const apple2 *a2, uint16_t addr)
{
    return a2->sp.rom[addr & 0xFF];
}

uint8_t sp_io(apple2 *a2, uint8_t reg, bool write)
{
    smartport *sp = &a2->sp;
    if (write)
        return 0;
    switch (reg) {
    case 0: return prodos_call(a2);
    case 1: return sp->x_out;
    case 2: return sp->y_out;
    case 3: return smartport_call(a2);
    default: return 0;
    }
}

// ---- public API -------------------------------------------------------------

void sp_init(smartport *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->listen_fd = sp->conn_fd = -1;
    sp->units = -1;
}

void sp_shutdown(smartport *sp)
{
    close_conn(sp);
    if (sp->listen_fd >= 0)
        close(sp->listen_fd);
    free(sp->rx);
    free(sp->resp);
    sp_init(sp);
}

bool a2_set_smartport(apple2 *a2, bool enabled, int port)
{
    smartport *sp = &a2->sp;
    if (!enabled) {
        sp_shutdown(sp);
        return true;
    }
    if (port <= 0 || port > 65535)
        return false;
    if (sp->enabled && sp->port == port)
        return true;
    sp_shutdown(sp);
    uint8_t *rx = malloc(SP_RX_CAP), *resp = malloc(SP_RESP_CAP);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!rx || !resp || fd < 0) {
        free(rx);
        free(resp);
        if (fd >= 0)
            close(fd);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // FujiNet-PC runs on this Mac
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
        close(fd);
        free(rx);
        free(resp);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sp->rx = rx;
    sp->resp = resp;
    sp->listen_fd = fd;
    sp->port = port;
    sp->enabled = true;
    build_rom(sp->rom);
    return true;
}

int a2_smartport_state(const apple2 *a2)
{
    const smartport *sp = &a2->sp;
    return !sp->enabled ? 0 : sp->conn_fd < 0 ? 1 : 2;
}

int a2_smartport_units(const apple2 *a2)
{
    return a2->sp.units > 0 ? a2->sp.units : 0;
}

bool a2_smartport_active(const apple2 *a2)
{
    const smartport *sp = &a2->sp;
    return sp->accessed && a2->frame_count - sp->last_access_frame < 6;
}
