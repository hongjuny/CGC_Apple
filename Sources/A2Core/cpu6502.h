// 6502 core: NMOS 6502 (documented opcodes, cycle counts incl. page-cross
// penalties, NMOS decimal mode; undocumented opcodes run as NOPs of the right
// length) or 65C02 when `cmos` is set. `rockwell` additionally enables the
// Rockwell bit instructions (BBR/BBS/RMB/SMB), which the 65C02 in the
// enhanced Apple IIe does not have.
#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>
#include <stdbool.h>

typedef uint8_t (*cpu_read_fn)(void *ctx, uint16_t addr);
typedef void (*cpu_write_fn)(void *ctx, uint16_t addr, uint8_t value);

enum {
    FLAG_C = 0x01, FLAG_Z = 0x02, FLAG_I = 0x04, FLAG_D = 0x08,
    FLAG_B = 0x10, FLAG_U = 0x20, FLAG_V = 0x40, FLAG_N = 0x80,
};

typedef struct cpu6502 {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    uint64_t cycles;        // total cycles executed since power-on
    bool irq_line;          // level-triggered
    bool nmi_pending;       // edge-triggered
    bool cmos;              // 65C02 instruction set and behaviour
    bool rockwell;          // with cmos: Rockwell bit instructions
    void *ctx;
    cpu_read_fn read;
    cpu_write_fn write;
} cpu6502;

void cpu_init(cpu6502 *cpu, void *ctx, cpu_read_fn read, cpu_write_fn write);
void cpu_reset(cpu6502 *cpu);
// Executes one instruction (or services one interrupt); returns cycles used.
int cpu_step(cpu6502 *cpu);

#endif
