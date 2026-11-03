#include "cpu6502.h"

// Base cycle counts for every opcode (NMOS 6502). Page-cross and branch
// penalties are added at runtime.
static const uint8_t base_cycles[256] = {
/*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
/* 0 */   7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
/* 1 */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/* 2 */   6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
/* 3 */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/* 4 */   6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
/* 5 */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/* 6 */   6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
/* 7 */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/* 8 */   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/* 9 */   2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
/* A */   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/* B */   2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
/* C */   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/* D */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/* E */   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/* F */   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

#define RD(a)     cpu->read(cpu->ctx, (uint16_t)(a))
#define WR(a, v)  cpu->write(cpu->ctx, (uint16_t)(a), (uint8_t)(v))

static inline uint16_t rd16(cpu6502 *cpu, uint16_t a)
{
    return (uint16_t)(RD(a) | (RD((uint16_t)(a + 1)) << 8));
}

// JMP (ind) and zero-page pointers wrap within the page.
static inline uint16_t rd16_wrap(cpu6502 *cpu, uint16_t a)
{
    uint16_t hi = (uint16_t)((a & 0xFF00) | ((a + 1) & 0x00FF));
    return (uint16_t)(RD(a) | (RD(hi) << 8));
}

static inline void push(cpu6502 *cpu, uint8_t v) { WR(0x100 | cpu->sp, v); cpu->sp--; }
static inline uint8_t pull(cpu6502 *cpu) { cpu->sp++; return RD(0x100 | cpu->sp); }

static inline void set_nz(cpu6502 *cpu, uint8_t v)
{
    cpu->p = (uint8_t)((cpu->p & ~(FLAG_N | FLAG_Z)) | (v & FLAG_N) | (v ? 0 : FLAG_Z));
}

static inline void set_flag(cpu6502 *cpu, uint8_t f, bool on)
{
    cpu->p = on ? (cpu->p | f) : (cpu->p & ~f);
}

void cpu_init(cpu6502 *cpu, void *ctx, cpu_read_fn read, cpu_write_fn write)
{
    *cpu = (cpu6502){0};
    cpu->ctx = ctx;
    cpu->read = read;
    cpu->write = write;
    cpu->sp = 0xFD;
    cpu->p = FLAG_U | FLAG_I;
}

void cpu_reset(cpu6502 *cpu)
{
    cpu->sp = (uint8_t)(cpu->sp - 3);
    cpu->p |= FLAG_I | FLAG_U;
    cpu->pc = rd16(cpu, 0xFFFC);
    cpu->nmi_pending = false;
    cpu->cycles += 7;
}

static void interrupt(cpu6502 *cpu, uint16_t vector, bool brk)
{
    push(cpu, cpu->pc >> 8);
    push(cpu, cpu->pc & 0xFF);
    push(cpu, (uint8_t)(((cpu->p | FLAG_U) & ~FLAG_B) | (brk ? FLAG_B : 0)));
    cpu->p |= FLAG_I;
    if (cpu->cmos)
        cpu->p &= ~FLAG_D;
    cpu->pc = rd16(cpu, vector);
}

// Returns the extra cycle the 65C02 spends in decimal mode.
static int adc(cpu6502 *cpu, uint8_t v)
{
    unsigned c = cpu->p & FLAG_C;
    unsigned a = cpu->a;
    if (cpu->p & FLAG_D) {
        unsigned lo = (a & 0x0F) + (v & 0x0F) + c;
        if (lo > 9) lo += 6;
        unsigned hi = (a >> 4) + (v >> 4) + (lo > 0x0F);
        set_flag(cpu, FLAG_Z, ((a + v + c) & 0xFF) == 0);
        set_flag(cpu, FLAG_N, hi & 8);
        set_flag(cpu, FLAG_V, ((a ^ (hi << 4)) & ~(a ^ v) & 0x80) != 0);
        if (hi > 9) hi += 6;
        set_flag(cpu, FLAG_C, hi > 15);
        cpu->a = (uint8_t)((hi << 4) | (lo & 0x0F));
        if (cpu->cmos) {
            set_nz(cpu, cpu->a);   // the 65C02 sets N and Z from the decimal result
            return 1;
        }
    } else {
        unsigned r = a + v + c;
        set_flag(cpu, FLAG_C, r > 0xFF);
        set_flag(cpu, FLAG_V, (~(a ^ v) & (a ^ r) & 0x80) != 0);
        cpu->a = (uint8_t)r;
        set_nz(cpu, cpu->a);
    }
    return 0;
}

static int sbc(cpu6502 *cpu, uint8_t v)
{
    unsigned borrow = (cpu->p & FLAG_C) ? 0 : 1;
    unsigned a = cpu->a;
    unsigned r = a - v - borrow;
    // Flags are always the binary-mode result on the NMOS part.
    set_flag(cpu, FLAG_C, r < 0x100);
    set_flag(cpu, FLAG_V, ((a ^ v) & (a ^ r) & 0x80) != 0);
    set_nz(cpu, (uint8_t)r);
    if (cpu->p & FLAG_D) {
        int lo = (int)(a & 0x0F) - (int)(v & 0x0F) - (int)borrow;
        int hi = (int)(a >> 4) - (int)(v >> 4);
        if (lo & 0x10) { lo -= 6; hi--; }
        if (hi & 0x10) hi -= 6;
        cpu->a = (uint8_t)((hi << 4) | (lo & 0x0F));
        if (cpu->cmos) {
            set_nz(cpu, cpu->a);
            return 1;
        }
    } else {
        cpu->a = (uint8_t)r;
    }
    return 0;
}

static inline void cmp(cpu6502 *cpu, uint8_t reg, uint8_t v)
{
    set_flag(cpu, FLAG_C, reg >= v);
    set_nz(cpu, (uint8_t)(reg - v));
}

static inline uint8_t asl(cpu6502 *cpu, uint8_t v) { set_flag(cpu, FLAG_C, v & 0x80); v <<= 1; set_nz(cpu, v); return v; }
static inline uint8_t lsr(cpu6502 *cpu, uint8_t v) { set_flag(cpu, FLAG_C, v & 1); v >>= 1; set_nz(cpu, v); return v; }
static inline uint8_t rol(cpu6502 *cpu, uint8_t v)
{
    uint8_t c = cpu->p & FLAG_C;
    set_flag(cpu, FLAG_C, v & 0x80);
    v = (uint8_t)((v << 1) | c);
    set_nz(cpu, v);
    return v;
}
static inline uint8_t ror(cpu6502 *cpu, uint8_t v)
{
    uint8_t c = cpu->p & FLAG_C;
    set_flag(cpu, FLAG_C, v & 1);
    v = (uint8_t)((v >> 1) | (c << 7));
    set_nz(cpu, v);
    return v;
}
static inline uint8_t inc(cpu6502 *cpu, uint8_t v) { v++; set_nz(cpu, v); return v; }
static inline uint8_t dec(cpu6502 *cpu, uint8_t v) { v--; set_nz(cpu, v); return v; }

// Length in bytes of an undocumented opcode, so execution stays aligned.
static int illegal_length(uint8_t op)
{
    switch (op & 0x0F) {
    case 0x2: return (op == 0x82 || op == 0xC2 || op == 0xE2) ? 2 : 1; // NOP #imm / JAM
    case 0x3: case 0x4: case 0x7: case 0x9: return 2;
    case 0xA: return 1;
    case 0xB: return (op & 0x10) ? 3 : 2;
    default:  return 3; // xC, xE, xF
    }
}

// Unused 65C02 opcodes are NOPs of fixed length and timing; returns cycles.
static int cmos_nop(cpu6502 *cpu, uint8_t op)
{
    switch (op) {
    case 0x44: cpu->pc += 1; return 3;
    case 0x54: case 0xD4: case 0xF4: cpu->pc += 1; return 4;
    case 0x5C: cpu->pc += 2; return 8;
    case 0xDC: case 0xFC: cpu->pc += 2; return 4;
    }
    if ((op & 0x0F) == 0x02) {   // $02, $22, ... $E2
        cpu->pc += 1;
        return 2;
    }
    return 1;                    // x3, x7, xB, xF
}

int cpu_step(cpu6502 *cpu)
{
    if (cpu->nmi_pending) {
        cpu->nmi_pending = false;
        interrupt(cpu, 0xFFFA, false);
        cpu->cycles += 7;
        return 7;
    }
    if (cpu->irq_line && !(cpu->p & FLAG_I)) {
        interrupt(cpu, 0xFFFE, false);
        cpu->cycles += 7;
        return 7;
    }

    uint8_t op = RD(cpu->pc++);
    int cycles = base_cycles[op];
    uint16_t ea = 0;
    uint8_t v;

#define IMM()  (ea = cpu->pc++)
#define ZP()   (ea = RD(cpu->pc++))
#define ZPX()  (ea = (uint8_t)(RD(cpu->pc++) + cpu->x))
#define ZPY()  (ea = (uint8_t)(RD(cpu->pc++) + cpu->y))
#define ABS()  (ea = rd16(cpu, cpu->pc), cpu->pc += 2)
#define ABSI(reg, penalty) do { uint16_t b_ = rd16(cpu, cpu->pc); cpu->pc += 2; \
        ea = (uint16_t)(b_ + (reg)); if ((penalty) && ((b_ ^ ea) & 0xFF00)) cycles++; } while (0)
#define INDX() (ea = rd16_wrap(cpu, (uint8_t)(RD(cpu->pc++) + cpu->x)))
#define INDY(penalty) do { uint16_t b_ = rd16_wrap(cpu, RD(cpu->pc++)); \
        ea = (uint16_t)(b_ + cpu->y); if ((penalty) && ((b_ ^ ea) & 0xFF00)) cycles++; } while (0)

    // Effective address for the regular ORA/AND/EOR/ADC/STA/LDA/CMP/SBC column layout.
#define ALU_EA(penalty) switch (op & 0x1F) { \
        case 0x01: INDX(); break; case 0x05: ZP(); break; case 0x09: IMM(); break; \
        case 0x0D: ABS(); break; case 0x11: INDY(penalty); break; case 0x15: ZPX(); break; \
        case 0x19: ABSI(cpu->y, penalty); break; case 0x1D: ABSI(cpu->x, penalty); break; }

    // Effective address for the shift/inc/dec column layout.
#define RMW(fn) do { switch (op & 0x1F) { case 0x06: ZP(); break; case 0x16: ZPX(); break; \
        case 0x0E: ABS(); break; case 0x1E: ABSI(cpu->x, 0); break; } \
        v = RD(ea); WR(ea, v); v = fn(cpu, v); WR(ea, v); } while (0)

#define BRANCH(cond) do { int8_t off_ = (int8_t)RD(cpu->pc++); if (cond) { \
        uint16_t t_ = (uint16_t)(cpu->pc + off_); cycles += ((t_ ^ cpu->pc) & 0xFF00) ? 2 : 1; cpu->pc = t_; } } while (0)

    if (cpu->cmos) {
        // Instructions added or changed on the 65C02. Anything not handled
        // here falls through to the common NMOS/CMOS implementation below.
        bool handled = true;
        switch (op) {
        case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
            cycles = 5;   // (zp) indirect
            ea = rd16_wrap(cpu, RD(cpu->pc++));
            switch (op >> 5) {
            case 0: cpu->a |= RD(ea); set_nz(cpu, cpu->a); break;
            case 1: cpu->a &= RD(ea); set_nz(cpu, cpu->a); break;
            case 2: cpu->a ^= RD(ea); set_nz(cpu, cpu->a); break;
            case 3: cycles += adc(cpu, RD(ea)); break;
            case 4: WR(ea, cpu->a); break;
            case 5: cpu->a = RD(ea); set_nz(cpu, cpu->a); break;
            case 6: cmp(cpu, cpu->a, RD(ea)); break;
            case 7: cycles += sbc(cpu, RD(ea)); break;
            }
            break;
        case 0x80: cycles = 2; BRANCH(true); break;                          // BRA
        case 0x89: v = RD(cpu->pc++); set_flag(cpu, FLAG_Z, !(cpu->a & v)); break; // BIT #imm
        case 0x34: case 0x3C:                                                  // BIT zp,X / abs,X
            if (op == 0x34) { cycles = 4; ZPX(); } else { cycles = 4; ABSI(cpu->x, 1); }
            v = RD(ea);
            cpu->p = (uint8_t)((cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z)) | (v & (FLAG_N | FLAG_V)) | ((cpu->a & v) ? 0 : FLAG_Z));
            break;
        case 0x1A: cycles = 2; cpu->a++; set_nz(cpu, cpu->a); break;          // INC A
        case 0x3A: cycles = 2; cpu->a--; set_nz(cpu, cpu->a); break;          // DEC A
        case 0x5A: cycles = 3; push(cpu, cpu->y); break;                       // PHY
        case 0x7A: cycles = 4; cpu->y = pull(cpu); set_nz(cpu, cpu->y); break; // PLY
        case 0xDA: cycles = 3; push(cpu, cpu->x); break;                       // PHX
        case 0xFA: cycles = 4; cpu->x = pull(cpu); set_nz(cpu, cpu->x); break; // PLX
        case 0x64: cycles = 3; ZP();  WR(ea, 0); break;                        // STZ
        case 0x74: cycles = 4; ZPX(); WR(ea, 0); break;
        case 0x9C: cycles = 4; ABS(); WR(ea, 0); break;
        case 0x9E: cycles = 5; ABSI(cpu->x, 0); WR(ea, 0); break;
        case 0x04: case 0x0C: case 0x14: case 0x1C:                            // TSB / TRB
            if (op & 0x08) { cycles = 6; ABS(); } else { cycles = 5; ZP(); }
            v = RD(ea);
            set_flag(cpu, FLAG_Z, !(cpu->a & v));
            WR(ea, (op & 0x10) ? (v & ~cpu->a) : (v | cpu->a));
            break;
        case 0x7C: {                                                           // JMP (abs,X)
            uint16_t b = (uint16_t)(rd16(cpu, cpu->pc) + cpu->x);
            cpu->pc = rd16(cpu, b);
            cycles = 6;
            break;
        }
        case 0x6C:                                                             // JMP (abs), no page bug
            cpu->pc = rd16(cpu, rd16(cpu, cpu->pc));
            cycles = 6;
            break;
        case 0x1E: case 0x3E: case 0x5E: case 0x7E: {
            // Shifts abs,X take 6 cycles (+1 on page cross) on the 65C02.
            uint16_t b = rd16(cpu, cpu->pc);
            cycles = (((uint16_t)(b + cpu->x) ^ b) & 0xFF00) ? 7 : 6;
            handled = false;
            break;
        }
        default:
            handled = false;
            break;
        }
        if (handled)
            goto done;
    }

    switch (op) {
    case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x15: case 0x19: case 0x1D:
        ALU_EA(1); cpu->a |= RD(ea); set_nz(cpu, cpu->a); break;
    case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x35: case 0x39: case 0x3D:
        ALU_EA(1); cpu->a &= RD(ea); set_nz(cpu, cpu->a); break;
    case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x55: case 0x59: case 0x5D:
        ALU_EA(1); cpu->a ^= RD(ea); set_nz(cpu, cpu->a); break;
    case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x75: case 0x79: case 0x7D:
        ALU_EA(1); cycles += adc(cpu, RD(ea)); break;
    case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB5: case 0xB9: case 0xBD:
        ALU_EA(1); cpu->a = RD(ea); set_nz(cpu, cpu->a); break;
    case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD5: case 0xD9: case 0xDD:
        ALU_EA(1); cmp(cpu, cpu->a, RD(ea)); break;
    case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF5: case 0xF9: case 0xFD:
        ALU_EA(1); cycles += sbc(cpu, RD(ea)); break;
    case 0x81: case 0x85: case 0x8D: case 0x91: case 0x95: case 0x99: case 0x9D:
        ALU_EA(0); WR(ea, cpu->a); break;

    case 0xA2: IMM(); goto ldx;
    case 0xA6: ZP(); goto ldx;
    case 0xB6: ZPY(); goto ldx;
    case 0xAE: ABS(); goto ldx;
    case 0xBE: ABSI(cpu->y, 1); goto ldx;
    ldx: cpu->x = RD(ea); set_nz(cpu, cpu->x); break;
    case 0xA0: IMM(); goto ldy;
    case 0xA4: ZP(); goto ldy;
    case 0xB4: ZPX(); goto ldy;
    case 0xAC: ABS(); goto ldy;
    case 0xBC: ABSI(cpu->x, 1); goto ldy;
    ldy: cpu->y = RD(ea); set_nz(cpu, cpu->y); break;
    case 0x86: ZP();  WR(ea, cpu->x); break;
    case 0x96: ZPY(); WR(ea, cpu->x); break;
    case 0x8E: ABS(); WR(ea, cpu->x); break;
    case 0x84: ZP();  WR(ea, cpu->y); break;
    case 0x94: ZPX(); WR(ea, cpu->y); break;
    case 0x8C: ABS(); WR(ea, cpu->y); break;

    case 0xE0: IMM(); cmp(cpu, cpu->x, RD(ea)); break;
    case 0xE4: ZP();  cmp(cpu, cpu->x, RD(ea)); break;
    case 0xEC: ABS(); cmp(cpu, cpu->x, RD(ea)); break;
    case 0xC0: IMM(); cmp(cpu, cpu->y, RD(ea)); break;
    case 0xC4: ZP();  cmp(cpu, cpu->y, RD(ea)); break;
    case 0xCC: ABS(); cmp(cpu, cpu->y, RD(ea)); break;
    case 0x24: ZP();  goto bit;
    case 0x2C: ABS(); goto bit;
    bit: v = RD(ea);
        cpu->p = (uint8_t)((cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z)) | (v & (FLAG_N | FLAG_V)) | ((cpu->a & v) ? 0 : FLAG_Z));
        break;

    case 0x06: case 0x16: case 0x0E: case 0x1E: RMW(asl); break;
    case 0x26: case 0x36: case 0x2E: case 0x3E: RMW(rol); break;
    case 0x46: case 0x56: case 0x4E: case 0x5E: RMW(lsr); break;
    case 0x66: case 0x76: case 0x6E: case 0x7E: RMW(ror); break;
    case 0xE6: case 0xF6: case 0xEE: case 0xFE: RMW(inc); break;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE: RMW(dec); break;
    case 0x0A: cpu->a = asl(cpu, cpu->a); break;
    case 0x2A: cpu->a = rol(cpu, cpu->a); break;
    case 0x4A: cpu->a = lsr(cpu, cpu->a); break;
    case 0x6A: cpu->a = ror(cpu, cpu->a); break;

    case 0xAA: cpu->x = cpu->a; set_nz(cpu, cpu->x); break;
    case 0x8A: cpu->a = cpu->x; set_nz(cpu, cpu->a); break;
    case 0xA8: cpu->y = cpu->a; set_nz(cpu, cpu->y); break;
    case 0x98: cpu->a = cpu->y; set_nz(cpu, cpu->a); break;
    case 0xBA: cpu->x = cpu->sp; set_nz(cpu, cpu->x); break;
    case 0x9A: cpu->sp = cpu->x; break;
    case 0xE8: cpu->x++; set_nz(cpu, cpu->x); break;
    case 0xCA: cpu->x--; set_nz(cpu, cpu->x); break;
    case 0xC8: cpu->y++; set_nz(cpu, cpu->y); break;
    case 0x88: cpu->y--; set_nz(cpu, cpu->y); break;

    case 0x48: push(cpu, cpu->a); break;
    case 0x68: cpu->a = pull(cpu); set_nz(cpu, cpu->a); break;
    case 0x08: push(cpu, cpu->p | FLAG_B | FLAG_U); break;
    case 0x28: cpu->p = (uint8_t)((pull(cpu) & ~FLAG_B) | FLAG_U); break;

    case 0x18: cpu->p &= ~FLAG_C; break;
    case 0x38: cpu->p |= FLAG_C; break;
    case 0x58: cpu->p &= ~FLAG_I; break;
    case 0x78: cpu->p |= FLAG_I; break;
    case 0xB8: cpu->p &= ~FLAG_V; break;
    case 0xD8: cpu->p &= ~FLAG_D; break;
    case 0xF8: cpu->p |= FLAG_D; break;

    case 0x10: BRANCH(!(cpu->p & FLAG_N)); break;
    case 0x30: BRANCH(cpu->p & FLAG_N); break;
    case 0x50: BRANCH(!(cpu->p & FLAG_V)); break;
    case 0x70: BRANCH(cpu->p & FLAG_V); break;
    case 0x90: BRANCH(!(cpu->p & FLAG_C)); break;
    case 0xB0: BRANCH(cpu->p & FLAG_C); break;
    case 0xD0: BRANCH(!(cpu->p & FLAG_Z)); break;
    case 0xF0: BRANCH(cpu->p & FLAG_Z); break;

    case 0x4C: cpu->pc = rd16(cpu, cpu->pc); break;
    case 0x6C: cpu->pc = rd16_wrap(cpu, rd16(cpu, cpu->pc)); break;
    case 0x20: {
        uint16_t t = rd16(cpu, cpu->pc);
        cpu->pc++;
        push(cpu, cpu->pc >> 8);
        push(cpu, cpu->pc & 0xFF);
        cpu->pc = t;
        break;
    }
    case 0x60: cpu->pc = pull(cpu); cpu->pc |= (uint16_t)(pull(cpu) << 8); cpu->pc++; break;
    case 0x40:
        cpu->p = (uint8_t)((pull(cpu) & ~FLAG_B) | FLAG_U);
        cpu->pc = pull(cpu);
        cpu->pc |= (uint16_t)(pull(cpu) << 8);
        break;
    case 0x00: cpu->pc++; interrupt(cpu, 0xFFFE, true); break;
    case 0xEA: break;

    default:
        if (!cpu->cmos) {
            cpu->pc = (uint16_t)(cpu->pc + illegal_length(op) - 1);
            break;
        }
        if (cpu->rockwell && (op & 0x0F) == 0x07) {          // RMBn / SMBn zp
            uint8_t zp = RD(cpu->pc++);
            uint8_t mask = (uint8_t)(1 << ((op >> 4) & 7));
            v = RD(zp);
            WR(zp, (op & 0x80) ? (v | mask) : (v & ~mask));
            cycles = 5;
            break;
        }
        if (cpu->rockwell && (op & 0x0F) == 0x0F) {          // BBRn / BBSn zp,rel
            uint8_t zp = RD(cpu->pc++);
            bool set = (RD(zp) >> ((op >> 4) & 7)) & 1;
            cycles = 5;
            BRANCH((op & 0x80) ? set : !set);
            break;
        }
        cycles = cmos_nop(cpu, op);
        break;
    }

done:
    cpu->cycles += (uint64_t)cycles;
    return cycles;
}
