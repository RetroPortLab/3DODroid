/* 3DODroid - ARM60 (ARMv3, 32-bit modes) interpreter.
 * Written from the ARM60 datasheet: banked registers, SPSRs, exceptions,
 * IRQ/FIQ lines, LDM/STM user-bank and CPSR-restore forms.
 * Timing: S/N cycle approximation (1 cycle per instruction, +loads/stores). */
#include "arm60.h"
#include <string.h>
#include <stdio.h>

Arm60 arm;
#define R arm.r

static int bank_idx(uint32_t m) {
    switch (m) {
    case M_SVC: return 1; case M_ABT: return 2; case M_UND: return 3;
    case M_IRQ: return 4; case M_FIQ: return 5; default: return 0;
    }
}

uint32_t arm_cpsr(void) {
    return (arm.n << 31) | (arm.z << 30) | (arm.c << 29) | (arm.v << 28) |
           (arm.i << 7) | (arm.f << 6) | arm.mode;
}

static void switch_mode(uint32_t nm) {
    uint32_t om = arm.mode;
    if (om == M_SYS) om = M_USR;
    uint32_t nmm = nm == M_SYS ? M_USR : nm;
    if (om == nmm) { arm.mode = nm; return; }
    int oi = bank_idx(om), ni = bank_idx(nmm);
    /* save current r13/r14/spsr */
    arm.bank_r13[oi] = R[13]; arm.bank_r14[oi] = R[14]; arm.bank_spsr[oi] = arm.spsr;
    /* r8-r12 swap for FIQ */
    if (om == M_FIQ && nmm != M_FIQ) {
        for (int k = 0; k < 5; k++) { arm.bank_fiq[k] = R[8 + k]; R[8 + k] = arm.bank_usr[k]; }
    } else if (om != M_FIQ && nmm == M_FIQ) {
        for (int k = 0; k < 5; k++) { arm.bank_usr[k] = R[8 + k]; R[8 + k] = arm.bank_fiq[k]; }
    }
    R[13] = arm.bank_r13[ni]; R[14] = arm.bank_r14[ni]; arm.spsr = arm.bank_spsr[ni];
    arm.mode = nm;
}

void arm_set_cpsr(uint32_t v) {
    arm.n = v >> 31; arm.z = (v >> 30) & 1; arm.c = (v >> 29) & 1; arm.v = (v >> 28) & 1;
    arm.i = (v >> 7) & 1; arm.f = (v >> 6) & 1;
    uint32_t m = v & 0x1F;
    if (m < 0x10) m |= 0x10;            /* 26-bit modes: treat as 32-bit equivalents */
    switch_mode(m);
}

static void exception(uint32_t vec, uint32_t mode, uint32_t ret, int disable_fiq) {
    if (vec == 0x04 && arm.log_undef && arm.log_undef-- > 0)
        fprintf(stderr, "[arm] undefined instruction %08x at %08x (mode %02x)\n", bus_read32(arm.cur_pc), arm.cur_pc, arm.mode);
    uint32_t old = arm_cpsr();
    switch_mode(mode);
    arm.spsr = old;
    R[14] = ret;
    arm.i = 1;
    if (disable_fiq) arm.f = 1;
    R[15] = vec;
}

void arm_reset(void) {
    memset(&arm, 0, sizeof arm);
    arm.mode = M_SVC; arm.i = 1; arm.f = 1;
    R[15] = 0;
}

static inline int cond_pass(uint32_t c) {
    switch (c) {
    case 0x0: return arm.z;            case 0x1: return !arm.z;
    case 0x2: return arm.c;            case 0x3: return !arm.c;
    case 0x4: return arm.n;            case 0x5: return !arm.n;
    case 0x6: return arm.v;            case 0x7: return !arm.v;
    case 0x8: return arm.c && !arm.z;  case 0x9: return !arm.c || arm.z;
    case 0xA: return arm.n == arm.v;   case 0xB: return arm.n != arm.v;
    case 0xC: return !arm.z && arm.n == arm.v;
    case 0xD: return arm.z || arm.n != arm.v;
    case 0xE: return 1;
    default:  return 0;
    }
}

static inline uint32_t ror(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline void set_nz(uint32_t v) { arm.n = v >> 31; arm.z = v == 0; }

static uint32_t operand2(uint32_t ins, uint32_t pc, int *carry, int *extra) {
    if (ins & 0x02000000) {
        uint32_t rot = ((ins >> 8) & 15) * 2, v = ror(ins & 0xFF, rot);
        *carry = rot ? v >> 31 : arm.c;
        return v;
    }
    uint32_t rm = ins & 15, type = (ins >> 5) & 3, amt;
    uint32_t val = rm == 15 ? pc + 8 : R[rm];
    int c = arm.c;
    if (ins & 0x10) {
        if (rm == 15) val += 4;
        amt = R[(ins >> 8) & 15] & 0xFF;
        (*extra)++;
        if (!amt) { *carry = c; return val; }
        switch (type) {
        case 0: if (amt < 32) { c = (val >> (32 - amt)) & 1; val <<= amt; } else { c = amt == 32 ? (int)(val & 1) : 0; val = 0; } break;
        case 1: if (amt < 32) { c = (val >> (amt - 1)) & 1; val >>= amt; } else { c = amt == 32 ? (int)(val >> 31) : 0; val = 0; } break;
        case 2: if (amt < 32) { c = ((int32_t)val >> (amt - 1)) & 1; val = (uint32_t)((int32_t)val >> amt); } else { c = val >> 31; val = c ? 0xFFFFFFFFu : 0; } break;
        case 3: val = ror(val, amt); c = val >> 31; break;
        }
        *carry = c; return val;
    }
    amt = (ins >> 7) & 31;
    switch (type) {
    case 0: if (amt) { c = (val >> (32 - amt)) & 1; val <<= amt; } break;
    case 1: if (!amt) amt = 32; c = (val >> (amt - 1)) & 1; val = amt == 32 ? 0 : val >> amt; break;
    case 2: if (!amt) amt = 32; c = ((int32_t)val >> (amt - 1)) & 1; val = (uint32_t)((int32_t)val >> (amt == 32 ? 31 : amt)); break;
    case 3: if (!amt) { int nc = val & 1; val = (val >> 1) | ((uint32_t)arm.c << 31); c = nc; } else { val = ror(val, amt); c = val >> 31; } break;
    }
    *carry = c; return val;
}

/* returns cycles */
static int exec_dp(uint32_t ins, uint32_t pc) {
    int op = (ins >> 21) & 15, s = (ins >> 20) & 1, extra = 0, sc;
    uint32_t rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
    uint32_t a = rn == 15 ? pc + 8 + ((!(ins & 0x02000000) && (ins & 0x10)) ? 4 : 0) : R[rn];
    uint32_t b = operand2(ins, pc, &sc, &extra);
    uint32_t res = 0; uint64_t t; int wb = 1, logical = 0;
    switch (op) {
    case 0x0: res = a & b; logical = 1; break;
    case 0x1: res = a ^ b; logical = 1; break;
    case 0x2: res = a - b; if (s) { arm.c = a >= b; arm.v = ((a ^ b) & (a ^ res)) >> 31; } break;
    case 0x3: res = b - a; if (s) { arm.c = b >= a; arm.v = ((b ^ a) & (b ^ res)) >> 31; } break;
    case 0x4: t = (uint64_t)a + b; res = (uint32_t)t; if (s) { arm.c = (uint32_t)(t >> 32); arm.v = (~(a ^ b) & (a ^ res)) >> 31; } break;
    case 0x5: t = (uint64_t)a + b + arm.c; res = (uint32_t)t; if (s) { arm.c = (uint32_t)(t >> 32); arm.v = (~(a ^ b) & (a ^ res)) >> 31; } break;
    case 0x6: t = (uint64_t)a - b - !arm.c; res = (uint32_t)t; if (s) { arm.c = (uint64_t)a >= (uint64_t)b + !arm.c; arm.v = ((a ^ b) & (a ^ res)) >> 31; } break;
    case 0x7: t = (uint64_t)b - a - !arm.c; res = (uint32_t)t; if (s) { arm.c = (uint64_t)b >= (uint64_t)a + !arm.c; arm.v = ((b ^ a) & (b ^ res)) >> 31; } break;
    case 0x8: wb = 0; res = a & b; logical = 1; break;
    case 0x9: wb = 0; res = a ^ b; logical = 1; break;
    case 0xA: wb = 0; res = a - b; arm.c = a >= b; arm.v = ((a ^ b) & (a ^ res)) >> 31; break;
    case 0xB: wb = 0; t = (uint64_t)a + b; res = (uint32_t)t; arm.c = (uint32_t)(t >> 32); arm.v = (~(a ^ b) & (a ^ res)) >> 31; break;
    case 0xC: res = a | b; logical = 1; break;
    case 0xD: res = b; logical = 1; break;
    case 0xE: res = a & ~b; logical = 1; break;
    case 0xF: res = ~b; logical = 1; break;
    }
    if (!wb && rd == 15 && s) {
        /* TSTP/TEQP/CMPP/CMNP (26-bit legacy): copy result to PSR */
        if (arm.mode != M_USR) arm_set_cpsr((arm_cpsr() & ~0xF00000FFu) | (res & 0xF00000FFu)); else arm.n = res >> 31, arm.z = (res >> 30) & 1, arm.c = (res >> 29) & 1, arm.v = (res >> 28) & 1;
        return 1 + extra;
    }
    if (s) { set_nz(res); if (logical) arm.c = sc; }
    if (wb) {
        if (rd == 15) {
            if (s && arm.mode != M_USR && arm.mode != M_SYS) { uint32_t sp = arm.spsr; arm_set_cpsr(sp); }
            R[15] = res & ~3u;
            return 3 + extra;
        }
        R[rd] = res;
    }
    R[15] = pc + 4;
    return 1 + extra;
}

static int exec_mul(uint32_t ins, uint32_t pc) {
    uint32_t rd = (ins >> 16) & 15, rn = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
    uint32_t r = R[rm] * R[rs];
    if (ins & 0x00200000) r += R[rn];
    R[rd] = r;
    if (ins & 0x00100000) set_nz(r);       /* C meaningless on ARM60, V unaffected */
    R[15] = pc + 4;
    /* booth: 2 bits per cycle, early terminate */
    uint32_t m = R[rs]; int c = 1; while (m && c < 16) { m >>= 2; c++; }
    return c;
}

static int exec_sdt(uint32_t ins, uint32_t pc) {
    uint32_t rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
    int p = (ins >> 24) & 1, u = (ins >> 23) & 1, b = (ins >> 22) & 1, w = (ins >> 21) & 1, l = (ins >> 20) & 1;
    uint32_t off;
    if (ins & 0x02000000) {
        if (ins & 0x10) {                       /* undefined on ARM */
            exception(0x04, M_UND, pc + 4, 0); return 3;
        }
        int dc, ex = 0; off = operand2(ins & ~0x02000000u, pc, &dc, &ex);
    } else off = ins & 0xFFF;
    uint32_t base = rn == 15 ? pc + 8 : R[rn];
    uint32_t nb = u ? base + off : base - off;
    uint32_t addr = p ? nb : base;
    R[15] = pc + 4;
    if (l) {
        uint32_t v;
        if (b) v = bus_read8(addr);
        else v = ror(bus_read32(addr & ~3u), (addr & 3) * 8);
        if (arm.abort_req) { arm.abort_req = 0; exception(0x10, M_ABT, pc + 8, 0); return 4; }
        if (!p || w) R[rn] = nb;
        if (rd == 15) { R[15] = v & ~3u; return 5; }
        R[rd] = v;
        return 3;
    }
    uint32_t v = rd == 15 ? pc + 12 : R[rd];
    if (b) bus_write8(addr, (uint8_t)v); else bus_write32(addr & ~3u, v);
    if (arm.trap_taken) { arm.trap_taken = 0; return 3; }
    if (arm.abort_req) { arm.abort_req = 0; exception(0x10, M_ABT, pc + 8, 0); return 4; }
    if (!p || w) R[rn] = nb;
    return 2;
}

static int exec_bdt(uint32_t ins, uint32_t pc) {
    uint32_t rn = (ins >> 16) & 15, list = ins & 0xFFFF;
    int p = (ins >> 24) & 1, u = (ins >> 23) & 1, sbit = (ins >> 22) & 1, w = (ins >> 21) & 1, l = (ins >> 20) & 1;
    int cnt = __builtin_popcount(list);
    if (!list) { cnt = 16; }
    uint32_t base = R[rn];
    uint32_t start = u ? base + (p ? 4 : 0) : base - cnt * 4 + (p ? 0 : 4);
    uint32_t nb = u ? base + cnt * 4 : base - cnt * 4;
    uint32_t a = start;
    R[15] = pc + 4;
    /* user bank transfer when S set and (store, or load without r15) */
    int userbank = sbit && !(l && (list & 0x8000));
    uint32_t saved_mode = arm.mode;
    if (l) {
        if (w) R[rn] = nb;
        if (userbank) switch_mode(M_USR);
        for (int i = 0; i < 16; i++) if (list & (1u << i)) {
            uint32_t v = bus_read32(a); a += 4;
            if (arm.abort_req) { arm.abort_req = 0; if (userbank) switch_mode(saved_mode); exception(0x10, M_ABT, pc + 8, 0); return 4; }
            if (i == 15) {
                if (sbit && arm.mode != M_USR && arm.mode != M_SYS) arm_set_cpsr(arm.spsr);
                R[15] = v & ~3u;
            } else R[i] = v;
        }
        if (userbank) switch_mode(saved_mode);
        return 2 + cnt + ((list & 0x8000) ? 2 : 0);
    }
    if (userbank) switch_mode(M_USR);
    int first = 1;
    for (int i = 0; i < 16; i++) if (list & (1u << i)) {
        uint32_t v = i == 15 ? pc + 12 : R[i];
        if (i == (int)rn && !first && w) v = nb;   /* base written back before later stores */
        bus_write32(a, v); a += 4; first = 0;
    }
    if (userbank) switch_mode(saved_mode);
    if (w) R[rn] = nb;
    return 1 + cnt;
}

/* ARMv4 halfword/signed transfers: not on a real ARM60, only enabled for
 * the CPU self-test (compiled with a v4 toolchain). */
static int exec_hdt(uint32_t ins, uint32_t pc) {
    uint32_t rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
    int p = (ins >> 24) & 1, u = (ins >> 23) & 1, w = (ins >> 21) & 1, l = (ins >> 20) & 1, sh = (ins >> 5) & 3;
    uint32_t off = (ins & 0x00400000) ? (((ins >> 4) & 0xF0) | (ins & 15)) : R[ins & 15];
    uint32_t base = rn == 15 ? pc + 8 : R[rn], nb = u ? base + off : base - off, a = p ? nb : base;
    R[15] = pc + 4;
    if (l) {
        uint32_t v;
        if (sh == 2) v = (uint32_t)(int32_t)(int8_t)bus_read8(a);
        else { uint32_t w32 = bus_read32(a & ~3u); uint32_t h = (a & 2) ? w32 >> 16 : w32 & 0xFFFF; v = sh == 3 ? (uint32_t)(int32_t)(int16_t)h : h; }
        if (!p || w) R[rn] = nb;
        R[rd] = v; return 3;
    }
    bus_write8(a, (uint8_t)R[rd]); bus_write8(a + 1, (uint8_t)(R[rd] >> 8));
    if (!p || w) R[rn] = nb;
    return 2;
}

static int exec_one(void) {
    uint32_t pc = R[15];
    arm.cur_pc = pc;
    uint32_t ins = bus_fetch(pc);
    if (!cond_pass(ins >> 28)) { R[15] = pc + 4; return 1; }
    switch ((ins >> 25) & 7) {
    case 0:
        if ((ins & 0x0FC000F0) == 0x00000090) return exec_mul(ins, pc);
        if (arm.v4 && (ins & 0x0F8000F0) == 0x00800090) {           /* UMULL/SMULL/UMLAL/SMLAL */
            uint32_t hi = (ins >> 16) & 15, lo = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
            uint64_t r = (ins & 0x00400000) ? (uint64_t)((int64_t)(int32_t)R[rm] * (int32_t)R[rs]) : (uint64_t)R[rm] * R[rs];
            if (ins & 0x00200000) r += ((uint64_t)R[hi] << 32) | R[lo];
            R[lo] = (uint32_t)r; R[hi] = (uint32_t)(r >> 32);
            if (ins & 0x00100000) { arm.n = (uint32_t)(r >> 63); arm.z = r == 0; }
            R[15] = pc + 4; return 4;
        }
        if ((ins & 0x0FB00FF0) == 0x01000090) {             /* SWP */
            uint32_t rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15, ad = R[rn];
            if (ins & 0x00400000) { uint8_t t = bus_read8(ad); bus_write8(ad, (uint8_t)R[rm]); R[rd] = t; }
            else { uint32_t t = ror(bus_read32(ad & ~3u), (ad & 3) * 8); bus_write32(ad & ~3u, R[rm]); R[rd] = t; }
            R[15] = pc + 4; return 4;
        }
        if ((ins & 0x0FBF0FFF) == 0x010F0000) {             /* MRS */
            R[(ins >> 12) & 15] = (ins & 0x00400000) ? arm.spsr : arm_cpsr();
            R[15] = pc + 4; return 1;
        }
        if ((ins & 0x0DB0F000) == 0x0120F000) {             /* MSR */
            uint32_t v;
            if (ins & 0x02000000) v = ror(ins & 0xFF, ((ins >> 8) & 15) * 2); else v = R[ins & 15];
            uint32_t mask = 0;
            if (ins & 0x00080000) mask |= 0xF0000000u;
            if ((ins & 0x00010000) && arm.mode != M_USR) mask |= 0xFFu;
            if (ins & 0x00400000) arm.spsr = (arm.spsr & ~mask) | (v & mask);
            else arm_set_cpsr((arm_cpsr() & ~mask) | (v & mask));
            R[15] = pc + 4; return 1;
        }
        if ((ins & 0x0E000090) == 0x00000090) {
            if (arm.v4) return exec_hdt(ins, pc);
            exception(0x04, M_UND, pc + 4, 0); return 3;
        }
        if (arm.v4 && (ins & 0x0FFFFFF0) == 0x012FFF10) { R[15] = R[ins & 15] & ~1u; return 3; }   /* BX */
        return exec_dp(ins, pc);
    case 1:
        if ((ins & 0x0DB0F000) == 0x0120F000 || (ins & 0x0FB00000) == 0x03200000) {
            uint32_t v = ror(ins & 0xFF, ((ins >> 8) & 15) * 2), mask = 0;
            if (ins & 0x00080000) mask |= 0xF0000000u;
            if ((ins & 0x00010000) && arm.mode != M_USR) mask |= 0xFFu;
            if (ins & 0x00400000) arm.spsr = (arm.spsr & ~mask) | (v & mask);
            else arm_set_cpsr((arm_cpsr() & ~mask) | (v & mask));
            R[15] = pc + 4; return 1;
        }
        return exec_dp(ins, pc);
    case 2: case 3: return exec_sdt(ins, pc);
    case 4: return exec_bdt(ins, pc);
    case 5: {
        int32_t off = (int32_t)(ins << 8) >> 6;
        if (ins & 0x01000000) R[14] = pc + 4;
        R[15] = pc + 8 + off; return 3;
    }
    case 6: exception(0x04, M_UND, pc + 4, 0); return 3;   /* coprocessor: none fitted */
    case 7:
        if (ins & 0x01000000) { exception(0x08, M_SVC, pc + 4, 0); return 3; }
        exception(0x04, M_UND, pc + 4, 0); return 3;
    }
    return 1;
}

int arm_run(int budget) {
    int done = 0;
    while (done < budget && !arm.halted) {
        if (arm.fiq_line && !arm.f) { exception(0x1C, M_FIQ, R[15] + 4, 1); done += 3; }
        else if (arm.irq_line && !arm.i) { exception(0x18, M_IRQ, R[15] + 4, 0); done += 3; }
        uint32_t before = R[15];
        if (arm.watch_hook && before == arm.watch_pc) arm.watch_hook();
        done += exec_one();
        if (arm.trace_hook && R[15] != before + 4) arm.trace_hook(before, R[15]);
    }
    arm.cycles += done;
    return done;
}
