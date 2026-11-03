// Runs Klaus Dormann's 6502_functional_test.bin or
// 65C02_extended_opcodes_test.bin against the CPU core.
// Usage: cputest <test.bin> [success_pc_hex] [65c02]
//   "65c02" selects the 65C02 with Rockwell bit instructions, as the
//   prebuilt extended-opcodes test expects (success trap $24F1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Sources/A2Core/cpu6502.h"

static uint8_t mem[65536];

static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s 6502_functional_test.bin [success_pc_hex]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(mem, 1, sizeof mem, f);
    fclose(f);
    unsigned success = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 16) : 0x3469;

    cpu6502 cpu;
    cpu_init(&cpu, NULL, rd, wr);
    cpu.cmos = cpu.rockwell = argc > 3 && strcmp(argv[3], "65c02") == 0;
    cpu.pc = 0x0400;
    printf("loaded %zu bytes, start $0400, success trap $%04X\n", n, success);

    for (;;) {
        uint16_t pc = cpu.pc;
        cpu_step(&cpu);
        if (cpu.pc == pc) {
            printf("trap at $%04X after %llu cycles: %s\n", pc,
                   (unsigned long long)cpu.cycles, pc == success ? "PASS" : "FAIL");
            return pc == success ? 0 : 1;
        }
    }
}
