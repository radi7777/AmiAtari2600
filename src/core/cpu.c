/*
 * cpu.c - MOS 6507 emulation (NMOS 6502 instruction set incl. the
 * undocumented opcodes used by Atari 2600 software).
 *
 * Timing model: each bus access costs one cycle (done in bus.c); internal
 * and dummy cycles are added here with CYC(). Dummy cycles are placed
 * *before* the final write of an instruction so that TIA/RIOT writes land
 * on the correct cycle (important for WSYNC, RESPx, HMOVE ...).
 *
 * Flags are kept unpacked for speed (vbcc/68030: byte ops on separate
 * variables are cheaper than bit-fiddling on P for every ALU op).
 */
#include "cpu.h"
#include "bus.h"

Cpu cpu;

/* Unpacked flags */
static u8 fC, fZ, fI, fD, fV, fN;   /* fZ/fN hold a result byte: Z = (fZ==0), N = fN & 0x80 */

#define RD(a)     bus_read((u16)(a))
#define WR(a, v)  bus_write((u16)(a), (u8)(v))
#define CYC(n)    (a26_cycles += (n))

#define SETNZ(v)  (fZ = fN = (u8)(v))

u8 cpu_get_p(void)
{
    cpu.p = (u8)((fN & 0x80) | (fV ? 0x40 : 0) | 0x20 | (fD ? 0x08 : 0) |
                 (fI ? 0x04 : 0) | (fZ == 0 ? 0x02 : 0) | (fC ? 0x01 : 0));
    return cpu.p;
}

void cpu_set_p(u8 p)
{
    cpu.p = p;
    fN = p & 0x80;
    fV = (p & 0x40) != 0;
    fD = (p & 0x08) != 0;
    fI = (p & 0x04) != 0;
    fZ = (p & 0x02) ? 0 : 1;
    fC = p & 0x01;
}

u16 cpu_pc_now;

void cpu_reset(void)
{
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.jammed = 0;
    cpu_set_p(0x24);
    cpu.pc = (u16)(RD(0xFFFC) | (RD(0xFFFD) << 8));
}

/* ---- ALU helpers ------------------------------------------------------ */

static void op_adc(u8 v)
{
    unsigned a = cpu.a;
    if (fD) {
        unsigned lo = (a & 0x0F) + (v & 0x0F) + (fC ? 1 : 0);
        unsigned hi = (a & 0xF0) + (v & 0xF0);
        fZ = (u8)(a + v + (fC ? 1 : 0));          /* Z from binary result (NMOS) */
        if (lo > 9) { lo += 6; }
        if (lo > 0x0F) { hi += 0x10; }
        fN = (u8)hi;
        fV = ((~(a ^ v)) & (a ^ hi) & 0x80) != 0;
        if (hi > 0x90) { hi += 0x60; }
        fC = hi > 0xFF;
        cpu.a = (u8)((hi & 0xF0) | (lo & 0x0F));
    } else {
        unsigned r = a + v + (fC ? 1 : 0);
        fC = r > 0xFF;
        fV = ((~(a ^ v)) & (a ^ r) & 0x80) != 0;
        cpu.a = (u8)r;
        SETNZ(cpu.a);
    }
}

static void op_sbc(u8 v)
{
    unsigned a = cpu.a;
    unsigned r = a - v - (fC ? 0 : 1);
    fV = ((a ^ v) & (a ^ r) & 0x80) != 0;
    SETNZ((u8)r);                                  /* NMOS: flags from binary result */
    if (fD) {
        int lo = (int)(a & 0x0F) - (int)(v & 0x0F) - (fC ? 0 : 1);
        int hi = (int)(a & 0xF0) - (int)(v & 0xF0);
        if (lo < 0) { lo -= 6; hi -= 0x10; }
        if (hi < 0) { hi -= 0x60; }
        cpu.a = (u8)((hi & 0xF0) | (lo & 0x0F));
    } else {
        cpu.a = (u8)r;
    }
    fC = r < 0x100;
}

static u8 op_cmp(u8 reg, u8 v)
{
    unsigned t_ = (unsigned)reg - v;
    fC = t_ < 0x100;
    SETNZ((u8)t_);
    return 0;
}
#define CMP(reg, v) op_cmp((reg), (v))

static u8 op_asl(u8 v) { fC = v & 0x80; v <<= 1; SETNZ(v); return v; }
static u8 op_lsr(u8 v) { fC = v & 0x01; v >>= 1; SETNZ(v); return v; }
static u8 op_rol(u8 v) { u8 c = fC ? 1 : 0; fC = v & 0x80; v = (u8)((v << 1) | c); SETNZ(v); return v; }
static u8 op_ror(u8 v) { u8 c = fC ? 0x80 : 0; fC = v & 0x01; v = (u8)((v >> 1) | c); SETNZ(v); return v; }

/* ---- stack ------------------------------------------------------------ */
#define PUSH(v)  do { WR(0x100 | cpu.s, (v)); cpu.s--; } while (0)
#define PULL()   (cpu.s++, RD(0x100 | cpu.s))

/* ---- addressing modes (compute ea) ------------------------------------ */
#define EA_ZP()    ea = RD(pc++)
#define EA_ZPX()   ea = (u8)(RD(pc++) + cpu.x); CYC(1)
#define EA_ZPY()   ea = (u8)(RD(pc++) + cpu.y); CYC(1)
#define EA_ABS()   ea = RD(pc); ea |= (u16)(RD(pc + 1) << 8); pc += 2; cpu_pc_now = pc
/* indexed read: penalty cycle only on page cross */
#define EA_ABXR()  EA_ABS(); t = ea; ea = (u16)(ea + cpu.x); if ((t ^ ea) & 0xFF00) CYC(1)
#define EA_ABYR()  EA_ABS(); t = ea; ea = (u16)(ea + cpu.y); if ((t ^ ea) & 0xFF00) CYC(1)
/* indexed write/RMW: always the extra cycle */
#define EA_ABXW()  EA_ABS(); ea = (u16)(ea + cpu.x); CYC(1)
#define EA_ABYW()  EA_ABS(); ea = (u16)(ea + cpu.y); CYC(1)
#define EA_IZX()   t = (u8)(RD(pc++) + cpu.x); CYC(1); ea = RD(t); ea |= (u16)(RD((u8)(t + 1)) << 8)
#define EA_IZYR()  t = RD(pc++); ea = RD(t); ea |= (u16)(RD((u8)(t + 1)) << 8); t = ea; \
                   ea = (u16)(ea + cpu.y); if ((t ^ ea) & 0xFF00) CYC(1)
#define EA_IZYW()  t = RD(pc++); ea = RD(t); ea |= (u16)(RD((u8)(t + 1)) << 8); \
                   ea = (u16)(ea + cpu.y); CYC(1)

/* read-modify-write: read, dummy cycle, write */
#define RMW(expr)  do { v = RD(ea); CYC(1); v = (expr); WR(ea, v); } while (0)

#define BRANCH(cond) do { \
        s8 off_ = (s8)RD(pc++); \
        if (cond) { \
            t = (u16)(pc + off_); CYC(1); \
            if ((t ^ pc) & 0xFF00) CYC(1); \
            pc = t; \
        } \
    } while (0)

u32 cpu_run(u32 target)
{
    u16 pc = cpu.pc;
    u16 ea, t;
    u8  v, op;
    u32 count = 0;

    if (cpu.jammed) {
        a26_cycles = target;
        return 0;
    }

    while ((s32)(a26_cycles - target) < 0 && !a26_stop) {
        op = RD(pc++);
        cpu_pc_now = 0;         /* only absolute operands publish the PC */
        count++;
        switch (op) {
        /* ---------------- loads / stores ---------------- */
        case 0xA9: cpu.a = RD(pc++); SETNZ(cpu.a); break;                     /* LDA # */
        case 0xA5: EA_ZP();   cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xB5: EA_ZPX();  cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xAD: EA_ABS();  cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xBD: EA_ABXR(); cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xB9: EA_ABYR(); cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xA1: EA_IZX();  cpu.a = RD(ea); SETNZ(cpu.a); break;
        case 0xB1: EA_IZYR(); cpu.a = RD(ea); SETNZ(cpu.a); break;

        case 0xA2: cpu.x = RD(pc++); SETNZ(cpu.x); break;                     /* LDX */
        case 0xA6: EA_ZP();   cpu.x = RD(ea); SETNZ(cpu.x); break;
        case 0xB6: EA_ZPY();  cpu.x = RD(ea); SETNZ(cpu.x); break;
        case 0xAE: EA_ABS();  cpu.x = RD(ea); SETNZ(cpu.x); break;
        case 0xBE: EA_ABYR(); cpu.x = RD(ea); SETNZ(cpu.x); break;

        case 0xA0: cpu.y = RD(pc++); SETNZ(cpu.y); break;                     /* LDY */
        case 0xA4: EA_ZP();   cpu.y = RD(ea); SETNZ(cpu.y); break;
        case 0xB4: EA_ZPX();  cpu.y = RD(ea); SETNZ(cpu.y); break;
        case 0xAC: EA_ABS();  cpu.y = RD(ea); SETNZ(cpu.y); break;
        case 0xBC: EA_ABXR(); cpu.y = RD(ea); SETNZ(cpu.y); break;

        case 0x85: EA_ZP();   WR(ea, cpu.a); break;                            /* STA */
        case 0x95: EA_ZPX();  WR(ea, cpu.a); break;
        case 0x8D: EA_ABS();  WR(ea, cpu.a); break;
        case 0x9D: EA_ABXW(); WR(ea, cpu.a); break;
        case 0x99: EA_ABYW(); WR(ea, cpu.a); break;
        case 0x81: EA_IZX();  WR(ea, cpu.a); break;
        case 0x91: EA_IZYW(); WR(ea, cpu.a); break;

        case 0x86: EA_ZP();   WR(ea, cpu.x); break;                            /* STX */
        case 0x96: EA_ZPY();  WR(ea, cpu.x); break;
        case 0x8E: EA_ABS();  WR(ea, cpu.x); break;

        case 0x84: EA_ZP();   WR(ea, cpu.y); break;                            /* STY */
        case 0x94: EA_ZPX();  WR(ea, cpu.y); break;
        case 0x8C: EA_ABS();  WR(ea, cpu.y); break;

        /* ---------------- transfers ---------------- */
        case 0xAA: cpu.x = cpu.a; SETNZ(cpu.x); CYC(1); break;                 /* TAX */
        case 0xA8: cpu.y = cpu.a; SETNZ(cpu.y); CYC(1); break;                 /* TAY */
        case 0x8A: cpu.a = cpu.x; SETNZ(cpu.a); CYC(1); break;                 /* TXA */
        case 0x98: cpu.a = cpu.y; SETNZ(cpu.a); CYC(1); break;                 /* TYA */
        case 0xBA: cpu.x = cpu.s; SETNZ(cpu.x); CYC(1); break;                 /* TSX */
        case 0x9A: cpu.s = cpu.x; CYC(1); break;                               /* TXS */

        /* ---------------- stack ---------------- */
        case 0x48: CYC(1); PUSH(cpu.a); break;                                 /* PHA */
        case 0x08: CYC(1); PUSH(cpu_get_p() | 0x10); break;                    /* PHP */
        case 0x68: CYC(2); cpu.a = PULL(); SETNZ(cpu.a); break;                /* PLA */
        case 0x28: CYC(2); cpu_set_p(PULL()); break;                           /* PLP */

        /* ---------------- logic ---------------- */
#define LOGIC(base, OPX) \
        case base + 0x09: v = RD(pc++); OPX; break; \
        case base + 0x05: EA_ZP();   v = RD(ea); OPX; break; \
        case base + 0x15: EA_ZPX();  v = RD(ea); OPX; break; \
        case base + 0x0D: EA_ABS();  v = RD(ea); OPX; break; \
        case base + 0x1D: EA_ABXR(); v = RD(ea); OPX; break; \
        case base + 0x19: EA_ABYR(); v = RD(ea); OPX; break; \
        case base + 0x01: EA_IZX();  v = RD(ea); OPX; break; \
        case base + 0x11: EA_IZYR(); v = RD(ea); OPX; break;

        LOGIC(0x00, cpu.a |= v; SETNZ(cpu.a))                                  /* ORA */
        LOGIC(0x20, cpu.a &= v; SETNZ(cpu.a))                                  /* AND */
        LOGIC(0x40, cpu.a ^= v; SETNZ(cpu.a))                                  /* EOR */
        LOGIC(0x60, op_adc(v))                                                 /* ADC */
        LOGIC(0xC0, CMP(cpu.a, v))                                             /* CMP */
        LOGIC(0xE0, op_sbc(v))                                                 /* SBC */
        case 0xEB: v = RD(pc++); op_sbc(v); break;                             /* SBC # (illegal) */

        case 0x24: EA_ZP();  v = RD(ea); fN = v; fV = (v & 0x40) != 0; fZ = v & cpu.a; break; /* BIT */
        case 0x2C: EA_ABS(); v = RD(ea); fN = v; fV = (v & 0x40) != 0; fZ = v & cpu.a; break;

        case 0xE0: v = RD(pc++); CMP(cpu.x, v); break;                         /* CPX */
        case 0xE4: EA_ZP();  v = RD(ea); CMP(cpu.x, v); break;
        case 0xEC: EA_ABS(); v = RD(ea); CMP(cpu.x, v); break;
        case 0xC0: v = RD(pc++); CMP(cpu.y, v); break;                         /* CPY */
        case 0xC4: EA_ZP();  v = RD(ea); CMP(cpu.y, v); break;
        case 0xCC: EA_ABS(); v = RD(ea); CMP(cpu.y, v); break;

        /* ---------------- inc / dec ---------------- */
        case 0xE6: EA_ZP();   RMW((u8)(v + 1)); SETNZ(v); break;               /* INC */
        case 0xF6: EA_ZPX();  RMW((u8)(v + 1)); SETNZ(v); break;
        case 0xEE: EA_ABS();  RMW((u8)(v + 1)); SETNZ(v); break;
        case 0xFE: EA_ABXW(); RMW((u8)(v + 1)); SETNZ(v); break;
        case 0xC6: EA_ZP();   RMW((u8)(v - 1)); SETNZ(v); break;               /* DEC */
        case 0xD6: EA_ZPX();  RMW((u8)(v - 1)); SETNZ(v); break;
        case 0xCE: EA_ABS();  RMW((u8)(v - 1)); SETNZ(v); break;
        case 0xDE: EA_ABXW(); RMW((u8)(v - 1)); SETNZ(v); break;
        case 0xE8: cpu.x++; SETNZ(cpu.x); CYC(1); break;                       /* INX */
        case 0xCA: cpu.x--; SETNZ(cpu.x); CYC(1); break;                       /* DEX */
        case 0xC8: cpu.y++; SETNZ(cpu.y); CYC(1); break;                       /* INY */
        case 0x88: cpu.y--; SETNZ(cpu.y); CYC(1); break;                       /* DEY */

        /* ---------------- shifts ---------------- */
#define SHIFT(base, FN) \
        case base + 0x0A: cpu.a = FN(cpu.a); CYC(1); break; \
        case base + 0x06: EA_ZP();   RMW(FN(v)); break; \
        case base + 0x16: EA_ZPX();  RMW(FN(v)); break; \
        case base + 0x0E: EA_ABS();  RMW(FN(v)); break; \
        case base + 0x1E: EA_ABXW(); RMW(FN(v)); break;

        SHIFT(0x00, op_asl)
        SHIFT(0x20, op_rol)
        SHIFT(0x40, op_lsr)
        SHIFT(0x60, op_ror)

        /* ---------------- jumps ---------------- */
        case 0x4C: EA_ABS(); pc = ea; break;                                   /* JMP abs */
        case 0x6C:                                                             /* JMP (ind), page-wrap bug */
            EA_ABS();
            t = RD(ea);
            t |= (u16)(RD((ea & 0xFF00) | ((ea + 1) & 0x00FF)) << 8);
            pc = t;
            break;
        case 0x20:                                                             /* JSR: lo, dummy, push, push, hi */
            ea = RD(pc++);
            CYC(1);
            PUSH(pc >> 8);
            PUSH(pc & 0xFF);
            ea |= (u16)(RD(pc) << 8);
            pc = ea;
            break;
        case 0x60:                                                             /* RTS */
            CYC(2);
            pc = PULL();
            pc |= (u16)(PULL() << 8);
            CYC(1);
            pc++;
            break;
        case 0x40:                                                             /* RTI */
            CYC(2);
            cpu_set_p(PULL());
            pc = PULL();
            pc |= (u16)(PULL() << 8);
            break;
        case 0x00:                                                             /* BRK */
            pc++;
            CYC(1);
            PUSH(pc >> 8);
            PUSH(pc & 0xFF);
            PUSH(cpu_get_p() | 0x10);
            fI = 1;
            pc = RD(0xFFFE);
            pc |= (u16)(RD(0xFFFF) << 8);
            break;

        /* ---------------- branches ---------------- */
        case 0x10: BRANCH(!(fN & 0x80)); break;                                /* BPL */
        case 0x30: BRANCH(fN & 0x80); break;                                   /* BMI */
        case 0x50: BRANCH(!fV); break;                                         /* BVC */
        case 0x70: BRANCH(fV); break;                                          /* BVS */
        case 0x90: BRANCH(!fC); break;                                         /* BCC */
        case 0xB0: BRANCH(fC); break;                                          /* BCS */
        case 0xD0: BRANCH(fZ); break;                                          /* BNE */
        case 0xF0: BRANCH(!fZ); break;                                         /* BEQ */

        /* ---------------- flags ---------------- */
        case 0x18: fC = 0; CYC(1); break;
        case 0x38: fC = 1; CYC(1); break;
        case 0x58: fI = 0; CYC(1); break;
        case 0x78: fI = 1; CYC(1); break;
        case 0xB8: fV = 0; CYC(1); break;
        case 0xD8: fD = 0; CYC(1); break;
        case 0xF8: fD = 1; CYC(1); break;

        /* ---------------- NOPs (documented + undocumented) ---------------- */
        case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
            CYC(1); break;
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:                 /* NOP # */
            pc++; CYC(1); break;
        case 0x04: case 0x44: case 0x64:                                       /* NOP zp */
            EA_ZP(); (void)RD(ea); break;
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:      /* NOP zp,X */
            EA_ZPX(); (void)RD(ea); break;
        case 0x0C:                                                             /* NOP abs */
            EA_ABS(); (void)RD(ea); break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:      /* NOP abs,X */
            EA_ABXR(); (void)RD(ea); break;

        /* ---------------- undocumented combined ops ---------------- */
        /* LAX */
        case 0xA7: EA_ZP();   cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xB7: EA_ZPY();  cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xAF: EA_ABS();  cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xBF: EA_ABYR(); cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xA3: EA_IZX();  cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xB3: EA_IZYR(); cpu.a = cpu.x = RD(ea); SETNZ(cpu.a); break;
        case 0xAB: cpu.a = cpu.x = (u8)((cpu.a | 0xEE) & RD(pc++)); SETNZ(cpu.a); break; /* LXA (unstable) */
        /* SAX */
        case 0x87: EA_ZP();  WR(ea, cpu.a & cpu.x); break;
        case 0x97: EA_ZPY(); WR(ea, cpu.a & cpu.x); break;
        case 0x8F: EA_ABS(); WR(ea, cpu.a & cpu.x); break;
        case 0x83: EA_IZX(); WR(ea, cpu.a & cpu.x); break;

#define COMBO(base, EXPR) \
        case base + 0x07: EA_ZP();   RMW(EXPR); break; \
        case base + 0x17: EA_ZPX();  RMW(EXPR); break; \
        case base + 0x0F: EA_ABS();  RMW(EXPR); break; \
        case base + 0x1F: EA_ABXW(); RMW(EXPR); break; \
        case base + 0x1B: EA_ABYW(); RMW(EXPR); break; \
        case base + 0x03: EA_IZX();  RMW(EXPR); break; \
        case base + 0x13: EA_IZYW(); RMW(EXPR); break;

        /* SLO = ASL + ORA, RLA = ROL + AND, SRE = LSR + EOR, RRA = ROR + ADC */
        COMBO(0x00, (v = op_asl(v), cpu.a |= v, SETNZ(cpu.a), v))
        COMBO(0x20, (v = op_rol(v), cpu.a &= v, SETNZ(cpu.a), v))
        COMBO(0x40, (v = op_lsr(v), cpu.a ^= v, SETNZ(cpu.a), v))
        COMBO(0x60, (v = op_ror(v), op_adc(v), v))
        /* DCP = DEC + CMP, ISB = INC + SBC */
        COMBO(0xC0, (v = (u8)(v - 1), CMP(cpu.a, v), v))
        COMBO(0xE0, (v = (u8)(v + 1), op_sbc(v), v))

        case 0x0B: case 0x2B:                                                  /* ANC */
            cpu.a &= RD(pc++); SETNZ(cpu.a); fC = cpu.a & 0x80; break;
        case 0x4B:                                                             /* ALR/ASR */
            cpu.a &= RD(pc++); cpu.a = op_lsr(cpu.a); break;
        case 0x6B:                                                             /* ARR */
            cpu.a &= RD(pc++);
            if (fD) {
                u8 a0 = cpu.a;
                cpu.a = (u8)((a0 >> 1) | (fC ? 0x80 : 0));
                SETNZ(cpu.a);
                fV = ((a0 ^ cpu.a) & 0x40) != 0;
                if (((a0 & 0x0F) + (a0 & 0x01)) > 5)
                    cpu.a = (u8)((cpu.a & 0xF0) | ((cpu.a + 6) & 0x0F));
                fC = ((a0 & 0xF0) + (a0 & 0x10)) > 0x50;
                if (fC) cpu.a = (u8)(cpu.a + 0x60);
            } else {
                cpu.a = (u8)((cpu.a >> 1) | (fC ? 0x80 : 0));
                SETNZ(cpu.a);
                fC = cpu.a & 0x40;
                fV = ((cpu.a >> 6) ^ (cpu.a >> 5)) & 1;
            }
            break;
        case 0xCB:                                                             /* SBX/AXS */
            v = RD(pc++);
            t = (u16)((cpu.a & cpu.x) - v);
            fC = t < 0x100;
            cpu.x = (u8)t; SETNZ(cpu.x);
            break;
        case 0xBB:                                                             /* LAS */
            EA_ABYR(); cpu.a = cpu.x = cpu.s = (u8)(RD(ea) & cpu.s); SETNZ(cpu.a); break;
        case 0x9F: EA_ABYW(); WR(ea, cpu.a & cpu.x & ((ea >> 8) + 1)); break;  /* SHA abs,Y */
        case 0x93: EA_IZYW(); WR(ea, cpu.a & cpu.x & ((ea >> 8) + 1)); break;  /* SHA (zp),Y */
        case 0x9E: EA_ABYW(); WR(ea, cpu.x & ((ea >> 8) + 1)); break;          /* SHX */
        case 0x9C: EA_ABXW(); WR(ea, cpu.y & ((ea >> 8) + 1)); break;          /* SHY */
        case 0x9B: EA_ABYW(); cpu.s = cpu.a & cpu.x; WR(ea, cpu.s & ((ea >> 8) + 1)); break; /* TAS */
        case 0x8B: cpu.a = (u8)((cpu.a | 0xEE) & cpu.x & RD(pc++)); SETNZ(cpu.a); break; /* ANE */

        /* ---------------- KIL / JAM ---------------- */
        default:
            cpu.jammed = 1;
            cpu.pc = (u16)(pc - 1);
            a26_cycles = target;
            return count;
        }
    }
    cpu.pc = pc;
    return count;
}
