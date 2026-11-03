// Passport MIDI test (card in slot 2, $C0A0):
//  1. ACIA: master reset, 8N1 /16, then 200 bytes of Note On/Off messages sent
//     by polling TDRE. Checks the bytes arrive intact and at the MIDI rate
//     (31250 baud = 3125 bytes/s).
//  2. PTM: timer 1 continuous, 1000-cycle period, IRQ handler (via $3FE)
//     clears the flag the documented way (status read, then counter read).
// Usage: miditest <romdir>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

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
static void store(uint8_t value, uint16_t addr) { emit(5, 0xA9, value, 0x8D, addr & 0xFF, addr >> 8); }

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

static apple2 *boot(const char *romdir)
{
    apple2 *a2 = a2_create(A2_MODEL_IIPLUS);
    if (!load(a2, romdir, "Apple2_Plus.rom", a2_load_system_rom) ||
        !load(a2, romdir, "Apple2_Video.rom", a2_load_char_rom)) {
        fprintf(stderr, "ROMs not found in %s\n", romdir);
        exit(2);
    }
    a2_set_mockingboards(a2, 0);
    a2_set_midi_slot(a2, 2);
    a2_power_cycle(a2);
    for (int i = 0; i < 30; i++)
        a2_run_frame(a2);
    return a2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir\n", argv[0]);
        return 2;
    }
    int failures = 0;

    // ---- 1. ACIA transmit -------------------------------------------------
    enum { N = 200 };
    uint8_t expected[N];
    for (int i = 0; i < N; i += 4) {        // Note On / Note Off, plus active sensing
        expected[i] = (i & 4) ? 0x80 : 0x90;
        expected[i + 1] = (uint8_t)(60 + (i / 8) % 12);
        expected[i + 2] = (i & 4) ? 0 : 100;
        expected[i + 3] = 0xFE;
    }

    apple2 *a2 = boot(argv[1]);
    memcpy(a2->ram[0] + 0x7000, expected, N);
    code = a2->ram[0] + 0x6000;
    pc = 0;
    store(0x03, 0xC0A8);                    // ACIA master reset
    store(0x15, 0xC0A8);                    // /16, 8 bits, no parity, 1 stop, no IRQs
    emit(2, 0xA2, 0x00);                    // LDX #0
    int loop = pc;
    emit(3, 0xAD, 0xA8, 0xC0);              // wait: LDA $C0A8
    emit(2, 0x29, 0x02);                    //       AND #TDRE
    emit(2, 0xF0, 0xF9);                    //       BEQ wait
    emit(3, 0xBD, 0x00, 0x70);              // LDA $7000,X
    emit(3, 0x8D, 0xA9, 0xC0);              // STA $C0A9
    emit(1, 0xE8);                          // INX
    emit(2, 0xE0, N);                       // CPX #N
    emit(2, 0xD0, (uint8_t)(loop - (pc + 2)));
    emit(3, 0x4C, (0x6000 + pc) & 0xFF, (0x6000 + pc) >> 8);   // JMP *
    a2->cpu.pc = 0x6000;

    uint8_t got[N + 16];
    size_t count = 0;
    uint64_t start = a2->cpu.cycles, done = 0;
    for (int f = 0; f < 60 && count < N; f++) {
        a2_run_frame(a2);
        count += a2_take_midi(a2, got + count, sizeof got - count);
        if (count >= N && !done)
            done = a2->cpu.cycles;
    }
    double rate = done ? N / ((double)(done - start) / A2_CPU_HZ) : 0;
    bool bytes_ok = count == N && memcmp(got, expected, N) == 0;
    // Frame-granular measurement: the last byte is seen at the end of its frame.
    bool rate_ok = rate > 3125 * 0.93 && rate < 3125 * 1.02;
    printf("ACIA: %zu/%d bytes %s, %.0f bytes/s (MIDI 3125/s) %s\n", count, N,
           bytes_ok ? "intact" : "WRONG", rate, rate_ok ? "PASS" : "FAIL");
    failures += !bytes_ok + !rate_ok;
    a2_destroy(a2);

    // ---- 2. PTM timer 1 interrupts -----------------------------------------
    a2 = boot(argv[1]);
    code = a2->ram[0] + 0x6100;
    pc = 0;
    emit(3, 0xAD, 0xA1, 0xC0);              // LDA $C0A1 (status)
    emit(3, 0xAD, 0xA2, 0xC0);              // LDA $C0A2 (timer 1 MSB: clears the flag)
    emit(2, 0xE6, 0x06);                    // INC $06
    emit(2, 0xD0, 0x02);                    // BNE +2
    emit(2, 0xE6, 0x07);                    // INC $07
    emit(2, 0xA5, 0x45);                    // LDA $45
    emit(1, 0x40);                          // RTI
    a2->ram[0][0x3FE] = 0x00;
    a2->ram[0][0x3FF] = 0x61;
    a2->ram[0][0x06] = a2->ram[0][0x07] = 0;

    code = a2->ram[0] + 0x6000;
    pc = 0;
    emit(1, 0x78);                          // SEI
    store(0x01, 0xC0A1);                    // CR2: register 0 addresses CR1
    store(0x01, 0xC0A0);                    // CR1: hold timers in reset
    store(999 >> 8, 0xC0A2);                // timer 1 latch = 999 -> period 1000
    store(999 & 0xFF, 0xC0A3);
    store(0x42, 0xC0A0);                    // CR1: internal clock, IRQ enable, continuous, run
    emit(1, 0x58);                          // CLI
    emit(3, 0x4C, (0x6000 + pc) & 0xFF, (0x6000 + pc) >> 8);
    a2->cpu.pc = 0x6000;
    start = a2->cpu.cycles;
    for (int f = 0; f < 60; f++)
        a2_run_frame(a2);
    double seconds = (double)(a2->cpu.cycles - start) / A2_CPU_HZ;
    unsigned irqs = a2->ram[0][0x06] | (a2->ram[0][0x07] << 8);
    double irq_rate = irqs / seconds;
    bool irq_ok = irq_rate > A2_CPU_HZ / 1000 * 0.99 && irq_rate < A2_CPU_HZ / 1000 * 1.01;
    printf("PTM: %u IRQs in %.3f s = %.0f/s (expected %.0f/s) %s\n", irqs, seconds, irq_rate,
           A2_CPU_HZ / 1000, irq_ok ? "PASS" : "FAIL");
    failures += !irq_ok;
    a2_destroy(a2);
    return failures ? 1 : 0;
}
