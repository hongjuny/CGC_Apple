// Mockingboard test: a small 6502 program programs the left AY-3-8913 for a
// 440 Hz tone on channel A through VIA 1, and runs VIA 1 timer 1 free-running
// with an interrupt every 1000 cycles, counted by an IRQ handler reached
// through the ROM's IRQ vector ($3FE). Checks the tone frequency on the left
// channel, silence on the right, and the interrupt rate.
// Usage: mbtest <romdir>   (A2MODEL=iiee for the enhanced IIe)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a2internal.h"

static uint8_t *code;
static int pc;
static uint16_t base = 0xC400;   // card slot ROM space ($Cn00)

static void emit(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    while (n--)
        code[pc++] = (uint8_t)va_arg(ap, int);
    va_end(ap);
}
static void store(uint8_t value, uint16_t addr) { emit(5, 0xA9, value, 0x8D, addr & 0xFF, addr >> 8); }

// PSG register write through VIA 1: latch address (BDIR+BC1), then write (BDIR).
static void psg(uint8_t reg, uint8_t value)
{
    store(reg, (base | 0x01));
    store(0x07, (base | 0x00));
    store(0x04, (base | 0x00));
    store(value, (base | 0x01));
    store(0x06, (base | 0x00));
    store(0x04, (base | 0x00));
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
    bool enhanced = getenv("A2MODEL") && strcmp(getenv("A2MODEL"), "iiee") == 0;
    apple2 *a2 = a2_create(enhanced ? A2_MODEL_IIE_ENHANCED : A2_MODEL_IIPLUS);
    if (!load(a2, argv[1], enhanced ? "Apple2e_Enhanced.rom" : "Apple2_Plus.rom", a2_load_system_rom) ||
        !load(a2, argv[1], enhanced ? "Apple2e_Enhanced_Video.rom" : "Apple2_Video.rom", a2_load_char_rom)) {
        fprintf(stderr, "ROMs not found in %s\n", argv[1]);
        return 2;
    }
    // MBCOUNT = cards installed (default 1), MBSLOT = slot of the card under test (4 or 5).
    a2_set_mockingboards(a2, getenv("MBCOUNT") ? atoi(getenv("MBCOUNT")) : 1);
    if (getenv("MBSLOT"))
        base = (uint16_t)(0xC000 | (atoi(getenv("MBSLOT")) << 8));
    a2_power_cycle(a2);
    for (int i = 0; i < 30; i++)   // let the ROM finish its reset work
        a2_run_frame(a2);

    // IRQ handler at $6100: clear T1, count, restore A (saved in $45 by the ROM), RTI.
    code = a2->ram[0] + 0x6100;
    pc = 0;
    emit(3, 0xAD, 0x04, base >> 8);     // LDA $Cn04
    emit(2, 0xE6, 0x06);               // INC $06
    emit(2, 0xD0, 0x02);               // BNE +2
    emit(2, 0xE6, 0x07);               // INC $07
    emit(2, 0xA5, 0x45);               // LDA $45
    emit(1, 0x40);                     // RTI
    a2->ram[0][0x3FE] = 0x00;
    a2->ram[0][0x3FF] = 0x61;
    a2->ram[0][0x06] = a2->ram[0][0x07] = 0;

    code = a2->ram[0] + 0x6000;
    pc = 0;
    emit(1, 0x78);                     // SEI
    store(0x00, 0xC006);               // IIe: INTCXROM off, so slot 4 is the card (no-op on the II+)
    store(0xFF, (base | 0x03));               // DDRA: PSG data bus out
    store(0x07, (base | 0x02));               // DDRB: BC1, BDIR, /RESET out
    store(0x00, (base | 0x00));               // reset PSG
    store(0x04, (base | 0x00));               // release reset, bus inactive
    psg(0, 145);                       // 1022727 / (16 * 145) = 440.8 Hz
    psg(1, 0);
    psg(7, 0x3E);                      // tone A only
    psg(8, 0x0F);                      // channel A full volume
    store(0x40, (base | 0x0B));               // ACR: T1 free-running
    store(998 & 0xFF, (base | 0x04));         // period 998 + 2 = 1000 cycles
    store(998 >> 8, (base | 0x05));
    store(0xC0, (base | 0x0E));               // IER: enable T1
    emit(1, 0x58);                     // CLI
    emit(3, 0x4C, (0x6000 + pc) & 0xFF, (0x6000 + pc) >> 8);   // JMP *

    a2->cpu.pc = 0x6000;
    int16_t pcm[4096 * 2];
    while (a2_take_audio(a2, pcm, 4096)) {}

    // Run for one second, counting zero crossings after a 0.1 s settling time.
    int frames = 60, crossings[2] = {0, 0}, prev[2] = {0, 0};
    long peak[2] = {0, 0}, sample_index = 0;
    uint64_t start = a2->cpu.cycles;
    for (int f = 0; f < frames; f++) {
        a2_run_frame(a2);
        size_t n;
        while ((n = a2_take_audio(a2, pcm, 4096)) > 0) {
            for (size_t i = 0; i < n; i++, sample_index++) {
                if (sample_index < A2_AUDIO_RATE / 10)
                    continue;
                for (int c = 0; c < 2; c++) {
                    int v = pcm[i * 2 + c];
                    if (labs(v) > peak[c])
                        peak[c] = labs(v);
                    int s = v > 500 ? 1 : v < -500 ? -1 : 0;   // hysteresis
                    if (s && s != prev[c]) {
                        if (prev[c])
                            crossings[c]++;
                        prev[c] = s;
                    }
                }
            }
        }
    }
    double seconds = (double)(a2->cpu.cycles - start) / A2_CPU_HZ;
    double measured = (sample_index - A2_AUDIO_RATE / 10) / (double)A2_AUDIO_RATE;
    double freq = crossings[0] / 2.0 / measured;
    unsigned irqs = a2->ram[0][0x06] | (a2->ram[0][0x07] << 8);
    double irq_rate = irqs / seconds;

    printf("left:  %.1f Hz (peak %ld)\n", freq, peak[0]);
    printf("right: %d crossings (peak %ld)\n", crossings[1], peak[1]);
    printf("timer 1 IRQs: %u in %.3f s = %.0f/s (expected %.0f/s)\n",
           irqs, seconds, irq_rate, A2_CPU_HZ / 1000.0);

    bool tone_ok = freq > 435 && freq < 446 && peak[0] > 2000;
    bool right_ok = peak[1] < 500;
    bool irq_ok = irq_rate > A2_CPU_HZ / 1000.0 * 0.99 && irq_rate < A2_CPU_HZ / 1000.0 * 1.01;
    printf("tone %s, right silent %s, IRQ rate %s\n",
           tone_ok ? "PASS" : "FAIL", right_ok ? "PASS" : "FAIL", irq_ok ? "PASS" : "FAIL");
    a2_destroy(a2);
    return tone_ok && right_ok && irq_ok ? 0 : 1;
}
