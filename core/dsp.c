/* 3DO audio DSP.
 *
 * Instruction and register behaviour follows the FreeDO project's
 * description of the DSPP (www.freedo.org; see README for credits).
 *
 * Memory, as the DSP sees it (16-bit words):
 *   0x000-0x07F  EI: parameters written by the ARM (0x70-0x7C also act as
 *                "CPU supply" words for the DMA channels)
 *   0x0D0-0x0DE  input FIFO status      0x0E0-0x0E3  output FIFO status
 *   0x0EA noise  0x0EB audio status  0x0EC/0x0ED semaphore  0x0EE pc  0x0EF count
 *   0x0F0-0x0FC  input FIFO data (each read takes the next sample word)
 *   0x070-0x07C  input FIFO data, current word without advancing
 *   0x100-0x1FF  I: working memory (mirrored at 0x200)
 *   0x300-0x3EA  EO: results the ARM can read
 *   0x3EB-0x3EF  status / semaphore / interrupt / count reload (write)
 *   0x3F0-0x3F3  output FIFO data   0x3FE/0x3FF  DAC left/right
 */
#include "dsp.h"
#include "sys3do.h"
#include <string.h>
#include <stdio.h>

int dsp_log;

#define TICKS_PER_SAMPLE 568        /* DSP clock / 44.1 kHz, for the count register */
#define MAX_STEPS        8192       /* guard against a program that never sleeps */

typedef struct {
    uint32_t addr, len, idx;        /* current buffer: byte address, byte length, bytes done */
    uint32_t next_addr, next_len;   /* queued buffer */
} Fifo;

static struct {
    uint16_t code[1024];
    uint16_t mem[1024];             /* EI 0x00-0x7F, I 0x100-0x1FF, EO 0x300-0x3FF */
    uint8_t  supply[16];
    uint16_t sema_status, sema_data, int_word, audio_status;
    int16_t  count, reload;
    int      running, raise_fiq;
    uint16_t noise;
    Fifo     in[13], out[4];

    /* execution state (reset at the start of every sample frame) */
    uint16_t pc, rbase, ret;
    int      rmap;
    uint8_t  opmask;                /* request bits still allowed (MULT1..BS) */
    uint32_t acc;
    int      c, v, z, n, exact;
    int16_t  mult1, mult2, alu1, alu2;
} d;

/* ---------------------------------------------------------- audio ring -- */
#define RING 16384                  /* frames, ~370 ms */
static int16_t ring[RING * 2];
static uint32_t ring_w, ring_r;     /* one producer (emulation), one consumer (audio output) */

static void ring_put(int16_t l, int16_t r) {
    uint32_t w = ring_w, rd_ = __atomic_load_n(&ring_r, __ATOMIC_ACQUIRE);
    if (w - rd_ >= RING) return;                    /* full: host isn't draining */
    ring[(w % RING) * 2] = l;
    ring[(w % RING) * 2 + 1] = r;
    __atomic_store_n(&ring_w, w + 1, __ATOMIC_RELEASE);
}

int dsp_audio_available(void) {
    return (int)(__atomic_load_n(&ring_w, __ATOMIC_ACQUIRE) - __atomic_load_n(&ring_r, __ATOMIC_ACQUIRE));
}

int dsp_audio_take(int16_t *out, int max_frames) {
    uint32_t r = ring_r, avail = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE) - r;
    int n = (int)(avail < (uint32_t)max_frames ? avail : (uint32_t)max_frames);
    for (int i = 0; i < n; i++, r++) {
        out[i * 2] = ring[(r % RING) * 2];
        out[i * 2 + 1] = ring[(r % RING) * 2 + 1];
    }
    __atomic_store_n(&ring_r, r, __ATOMIC_RELEASE);
    return n;
}

/* ------------------------------------------------------------- DMA FIFOs -- */
static int chan_enabled(int bit) { return (clio_dma_enables() >> bit) & 1; }

static void in_complete(int ch) {
    Fifo *f = &d.in[ch];
    f->idx = 0;
    clio_raise(0, 1u << (16 + ch));
    if (f->next_addr && chan_enabled(ch)) { f->addr = f->next_addr; f->len = f->next_len; }
    else f->addr = 0;
}

static uint16_t in_take(int ch) {
    Fifo *f = &d.in[ch];
    if (f->addr && f->idx >= f->len) in_complete(ch);
    if (!f->addr) return 0;
    uint16_t v = mem_rd16(f->addr + f->idx);
    if (dsp_log && sys.frame >= (uint64_t)dsp_log) fprintf(stderr, "[fifo] ch%d %08x = %04x\n", ch, f->addr + f->idx, v);
    f->idx += 2;
    if (f->idx >= f->len) in_complete(ch);
    return v;
}

static uint16_t in_peek(int ch) {
    Fifo *f = &d.in[ch];
    return f->addr ? mem_rd16(f->addr + f->idx) : 0;
}

static void out_put(int ch, uint16_t v) {
    Fifo *f = &d.out[ch];
    if (!f->addr) return;
    if (f->idx < f->len) { mem_wr16(f->addr + f->idx, v); f->idx += 2; return; }
    f->idx = 0;
    clio_raise(0, 1u << (12 + ch));
    if (f->next_addr && chan_enabled(16 + ch)) { f->addr = f->next_addr; f->len = f->next_len; }
    else f->addr = 0;
}

static Fifo *fifo_at(uint32_t off) {
    int ch = (off >> 4) & 0xF;
    if ((off & 0x700) == 0x400) return ch < 13 ? &d.in[ch] : NULL;
    if ((off & 0x700) == 0x500) return ch < 4 ? &d.out[ch] : NULL;
    return NULL;
}

void dsp_fifo_write(uint32_t off, uint32_t v) {
    Fifo *f = fifo_at(off);
    if (!f) return;
    switch (off & 0xC) {
    case 0x0: f->addr = v; f->idx = 0; f->next_addr = 0; break;     /* new buffer cancels the queued one */
    case 0x4: f->len = v ? v + 4 : 0; f->next_len = 0; break;
    case 0x8: f->next_addr = v; break;
    case 0xC: f->next_len = v ? v + 4 : 0; break;
    }
}

uint32_t dsp_fifo_read(uint32_t off) {
    Fifo *f = fifo_at(off);
    if (!f) return 0;
    switch (off & 0xC) {
    case 0x0: return f->addr + f->idx;
    case 0x4: return f->len - f->idx;
    case 0x8: return f->next_addr;
    default:  return f->next_len;
    }
}

void dsp_fifo_stop(uint32_t mask) {
    for (int i = 0; i < 13; i++) if (mask & (1u << i)) memset(&d.in[i], 0, sizeof d.in[i]);
    for (int i = 0; i < 4; i++) if (mask & (1u << (16 + i))) memset(&d.out[i], 0, sizeof d.out[i]);
}

/* ------------------------------------------------------- DSP memory bus -- */
static uint16_t noise(void) {
    d.noise = (uint16_t)(d.noise * 25173u + 13849u);
    return d.noise;
}

static uint16_t rd(uint32_t a) {
    a &= 0x3FF;
    if (a >= 0xF0 && a <= 0xFC) {
        int ch = a - 0xF0;
        if (d.supply[ch]) { d.supply[ch] = 0; return d.mem[0x70 + ch]; }
        return in_take(ch);
    }
    if (a >= 0x70 && a <= 0x7C) {
        int ch = a - 0x70;
        if (d.supply[ch]) { d.supply[ch] = 0; return d.mem[a]; }
        return in_peek(ch);
    }
    if (a >= 0xD0 && a <= 0xDE) {
        int ch = a & 0xF;
        return (d.supply[ch] || (ch < 13 && d.in[ch].addr)) ? 2 : 0;
    }
    if (a >= 0xE0 && a <= 0xE3) return d.out[a & 3].addr ? 1 : 0;
    switch (a) {
    case 0xEA: return noise();
    case 0xEB: return d.audio_status;
    case 0xEC: return d.sema_status;
    case 0xED: return d.sema_data;
    case 0xEE: return d.pc;
    case 0xEF: return (uint16_t)d.count;
    }
    if (a >= 0x100 && a < 0x300) return d.mem[0x100 | (a & 0xFF)];
    return d.mem[a & 0x7F];
}

static void wr(uint32_t a, uint16_t v) {
    a &= 0x3FF;
    switch (a) {
    case 0x3EB: d.audio_status = v; return;
    case 0x3EC: d.sema_status |= 1; return;               /* DSP acknowledges the ARM */
    case 0x3ED: d.sema_data = v; d.sema_status = 4; return; /* DSP posts a value */
    case 0x3EE: d.int_word = v; d.raise_fiq = 1; return;
    case 0x3EF: d.reload = (int16_t)v; return;
    case 0x3F0: case 0x3F1: case 0x3F2: case 0x3F3: out_put(a & 3, v); return;
    case 0x3FD: return;                                    /* flush output FIFO */
    }
    if (a < 0x100) return;
    if (a < 0x300) d.mem[0x100 | (a & 0xFF)] = v;
    else d.mem[a] = v;
}

/* Register r (0-15) as a data address: bits 0-2 pass through, bit 3 picks
 * the 0x200 half, and the "register map" chooses what selects 0x100. */
static uint32_t reg_addr(int r) {
    int x = (r >> 2) & 1, y = (r >> 3) & 1, sel;
    switch (d.rmap) {
    case 4:  sel = y; break;
    case 5:  sel = !y; break;
    case 6:  sel = x & y; break;
    case 7:  sel = x | y; break;
    default: sel = x; break;
    }
    return ((uint32_t)(r & 7) | (uint32_t)sel << 8 | (uint32_t)y << 9) ^ d.rbase;
}

/* ------------------------------------------------------------- operands -- */
enum { RQ_BS = 1, RQ_ALU2 = 2, RQ_ALU1 = 4, RQ_MULT2 = 8, RQ_MULT1 = 16 };

static uint16_t imm(uint16_t w) {
    int32_t v = (int32_t)((uint32_t)w << 19) >> 19;            /* signed 13 bits */
    return (uint16_t)(v << ((w & 0x2000) ? 3 : 0));
}

/* The single source operand of a MOVE. */
static uint16_t operand1(void) {
    uint16_t w = d.code[d.pc++ & 0x3FF];
    uint16_t v;
    switch (w >> 13) {
    case 0: case 1: case 2: case 3:                             /* register list: uses R3 */
        v = rd(reg_addr((w >> 10) & 15));
        return (w & 0x4000) ? rd(v) : v;
    case 4:                                                     /* address */
        v = rd(w & 0x3FF);
        return (w & 0x400) ? rd(v) : v;
    case 5:                                                     /* one/two registers: uses R1 */
        v = rd(reg_addr(w & 15));
        return (w & 0x10) ? rd(v) : v;
    default:
        return imm(w);
    }
}

/* Fetch the operands of an ALU instruction. Each operand word supplies one
 * to three values; they fill MULT1, MULT2, ALU1, ALU2, BS in that order for
 * whichever the instruction needs. An operand left over after those is where
 * the result is written back. Returns the write-back address (0 = none). */
static uint32_t operands(int count, int req) {
    uint16_t ops[8];
    int n = 0;
    uint32_t wb = 0, wb_flagged = 0;
    if (count == 0) { if (!req) return 0; count = 4; }

    while (n < count) {
        uint16_t w = d.code[d.pc++ & 0x3FF];
        uint32_t a;
        switch (w >> 13) {
        case 0: case 1: case 2: case 3: {                       /* three registers: R3, R2, R1 */
            ops[n] = rd(reg_addr((w >> 10) & 15));
            if (w & 0x4000) ops[n] = rd(ops[n]);
            n++;
            ops[n] = rd(reg_addr((w >> 5) & 15));
            if (w & 0x200) ops[n] = rd(ops[n]);
            n++;
            wb = reg_addr(w & 15);
            ops[n] = rd(wb);
            if (w & 0x10) ops[n] = rd(ops[n]);
            n++;
            break;
        }
        case 4:                                                 /* address */
            wb = w & 0x3FF;
            ops[n] = rd(wb);
            if (w & 0x400) ops[n] = rd(ops[n]);
            n++;
            if (w & 0x800) wb_flagged = wb;
            break;
        case 5:                                                 /* one or two registers */
            if (w & 0x400) {
                a = reg_addr((w >> 5) & 15);
                if (w & 0x200) a = rd(a);
                ops[n++] = rd(a);
                wb = a;
                if (w & 0x1000) wb_flagged = a;
            }
            a = reg_addr(w & 15);
            if (w & 0x10) a = rd(a);
            ops[n++] = rd(a);
            wb = a;
            if (w & 0x800) wb_flagged = a;
            break;
        default:                                                /* immediate */
            ops[n] = imm(w);
            wb = ops[n++];
            break;
        }
    }

    req &= d.opmask;
    int i = 0;
    if (req & RQ_MULT1) d.mult1 = (int16_t)ops[i++];
    if (req & RQ_MULT2) d.mult2 = (int16_t)ops[i++];
    if (req & RQ_ALU1)  d.alu1 = (int16_t)ops[i++];
    if (req & RQ_ALU2)  d.alu2 = (int16_t)ops[i++];
    if (req & RQ_BS)    i++;                                    /* shift amount operand (unused) */
    if (n != i) return wb_flagged ? wb_flagged : wb;
    return wb_flagged;
}

/* ------------------------------------------------------------ execution -- */
static int add_c(uint32_t a, uint32_t b, uint32_t y) { return (((a & b) | (a & ~y) | (b & ~y)) >> 31) & 1; }
static int sub_c(uint32_t a, uint32_t b, uint32_t y) { return (((a & ~b) | (a & ~y) | (~b & ~y)) >> 31) & 1; }
static int add_v(uint32_t a, uint32_t b, uint32_t y) { return (((a & b & ~y) | (~a & ~b & y)) >> 31) & 1; }
static int sub_v(uint32_t a, uint32_t b, uint32_t y) { return (((a & ~b & ~y) | (~a & b & y)) >> 31) & 1; }

static int branch_taken(uint16_t w) {
    int m0 = (w >> 10) & 1, m1 = (w >> 11) & 1, fs = (w >> 12) & 1, mode = (w >> 13) & 3;
    if (mode == 3) {
        if (!fs) return (((d.n != d.v) || (d.z && m0)) != m1);  /* signed compare */
        if (!m1) return ((d.c && !d.z) != m0);                  /* unsigned compare */
        return (d.exact != m0);
    }
    if (!m0 && !m1) {                                           /* no flags chosen */
        if (mode != 1) return 0;
        int zero = d.z && d.exact;
        return fs ? !zero : zero;
    }
    int s0 = fs ? d.c : d.n, s1 = fs ? d.z : d.v;
    if (mode == 2) { s0 = !s0; s1 = !s1; }
    return (!m1 || s0) && (!m0 || s1);
}

static uint32_t mult_a(int alu) {                               /* multiplier output, A side */
    return (alu == 3 || alu == 5) ? (d.c ? (uint32_t)((int32_t)d.mult1 << 16) : 0)
                                  : (uint32_t)((int32_t)d.mult1 * (((int32_t)d.acc >> 15) & ~1));
}

static void alu_op(uint16_t w) {
    int bs = w & 15, alu = (w >> 4) & 15, muxb = (w >> 8) & 3, muxa = (w >> 10) & 3;
    int m2 = (w >> 12) & 1, count = (w >> 13) & 3;
    int req = 0;
    if (bs == 8) req |= RQ_BS;
    for (int side = 0; side < 2; side++) {
        int mux = side ? muxb : muxa;
        if (mux == 1) req |= RQ_ALU1;
        else if (mux == 2) req |= RQ_ALU2;
        else if (mux == 3) req |= RQ_MULT1 | (m2 ? RQ_MULT2 : 0);
    }
    uint32_t wb = operands(count, req);

    uint32_t a, b;
    switch (muxa) {
    case 0:  a = d.acc; break;
    case 1:  a = (uint32_t)((int32_t)d.alu1 << 16); break;
    case 2:  a = (uint32_t)((int32_t)d.alu2 << 16); break;
    default: a = m2 ? (uint32_t)((int32_t)d.mult1 * d.mult2 * 2) : mult_a(alu); break;
    }
    if (alu == 3 || alu == 5) b = (uint32_t)d.c << 16;          /* add/subtract with carry */
    else switch (muxb) {
    case 0:  b = d.acc; break;
    case 1:  b = (uint32_t)((int32_t)d.alu1 << 16); break;
    case 2:  b = (uint32_t)((int32_t)d.alu2 << 16); break;
    default: b = m2 ? (uint32_t)((int32_t)d.mult1 * d.mult2 * 2)
                    : (uint32_t)(((int32_t)d.mult1 * ((int32_t)d.acc >> 15)) & ~1); break;
    }

    uint32_t y;
    d.c = d.v = 0;
    switch (alu) {
    case 0:  y = a; break;
    case 1:  y = 0 - b; d.c = sub_c(0, b, y); d.v = sub_v(0, b, y); break;
    case 2: case 3: y = a + b; d.c = add_c(a, b, y); d.v = add_v(a, b, y); break;
    case 4: case 5: y = a - b; d.c = sub_c(a, b, y); d.v = sub_v(a, b, y); break;
    case 6:  y = a + 0x1000; d.c = add_c(a, 0x1000, y); d.v = add_v(a, 0x1000, y); break;
    case 7:  y = a - 0x1000; d.c = sub_c(a, 0x1000, y); d.v = sub_v(a, 0x1000, y); break;
    case 8:  y = a; break;
    case 9:  y = ~a; break;
    case 10: y = a & b; break;
    case 11: y = ~(a & b); break;
    case 12: y = a | b; break;
    case 13: y = ~(a | b); break;
    case 14: y = a ^ b; break;
    default: y = ~(a ^ b); break;
    }
    d.z = (y & 0xFFFF0000u) == 0;
    d.n = y >> 31;
    d.exact = (y & 0xF000) == 0;

    /* Barrel shifter. Logical ops (ALU >= 8) use the logical variants. */
    int shift = bs | ((alu & 8) << 1);
    switch (shift) {
    case 1: case 17: y <<= 1; break;
    case 2: case 18: y <<= 2; break;
    case 3: case 19: y <<= 3; break;
    case 4: case 20: y <<= 4; break;
    case 5: case 21: y <<= 5; break;
    case 6: case 22: y <<= 8; break;
    case 7: case 23:                                            /* clip on overflow */
        if (d.v) y = d.n ? 0x7FFFF000u : 0x80000000u;
        break;
    case 8: case 24: {                                          /* rotate left through carry */
        int out = y >> 31;
        y = ((y << 1) & 0xFFFE0000u) | (d.c ? 0x10000u : 0) | (y & 0xF000);
        d.c = out;
        break;
    }
    case 9:  y = (uint32_t)((int32_t)y >> 16); break;
    case 10: y = (uint32_t)((int32_t)y >> 8); break;
    case 11: y = (uint32_t)((int32_t)y >> 5); break;
    case 12: y = (uint32_t)((int32_t)y >> 4); break;
    case 13: y = (uint32_t)((int32_t)y >> 3); break;
    case 14: y = (uint32_t)((int32_t)y >> 2); break;
    case 15: y = (uint32_t)((int32_t)y >> 1); break;
    case 25: y >>= 16; break;
    case 26: y >>= 8; break;
    case 27: y >>= 5; break;
    case 28: y >>= 4; break;
    case 29: y >>= 3; break;
    case 30: y >>= 2; break;
    case 31: y >>= 1; break;
    }
    d.acc = y;
    if (wb) wr(wb, (uint16_t)((int32_t)y >> 16));
}

static int control_op(uint16_t w) {
    int op = (w >> 7) & 0xFF;
    if (op >= 64) { if (branch_taken(w)) d.pc = w & 0x3FF; return 1; }
    if (op >= 48) {                                             /* move to address */
        uint16_t v = operand1();
        uint32_t a = w & 0x3FF;
        if (w & 0x400) a = rd(a);
        wr(a, v);
        return 1;
    }
    if (op >= 32) {                                             /* move to register */
        uint16_t v = operand1();
        uint32_t a = reg_addr(w & 15);
        if (w & 0x10) a = rd(a);
        wr(a, v);
        return 1;
    }
    if (op >= 24) { d.pc = w & 0x3FF; return 1; }               /* branch */
    if (op >= 16) { d.ret = d.pc; d.pc = w & 0x3FF; return 1; } /* jump to subroutine */
    if (op >= 8)  { d.pc = w & 0x3FF; return 1; }               /* jump */
    switch (op) {
    case 1: d.pc = (d.acc >> 16) & 0x3FF; break;                /* jump to accumulator */
    case 2: d.rbase = (uint16_t)((w & 0x3F) << 2); break;
    case 3: d.rmap = w & 7; break;
    case 4: d.pc = d.ret; break;
    case 5: d.opmask = (uint8_t)~(w & 0x1F); break;
    case 7: return 0;                                           /* sleep until the next frame */
    }
    return 1;
}

void dsp_sample(void) {
    if (d.running) {
        d.pc = 0; d.rbase = 0; d.rmap = 0; d.opmask = 0xFF;
        d.acc = 0; d.c = d.v = d.z = d.n = d.exact = 0;
        for (int steps = 0; steps < MAX_STEPS; steps++) {
            uint16_t w = d.code[d.pc++ & 0x3FF];
            d.pc &= 0x3FF;
            if (w & 0x8000) { if (!control_op(w)) break; }
            else alu_op(w);
            d.pc &= 0x3FF;
        }
        if (d.raise_fiq) { d.raise_fiq = 0; clio_raise(0, 1u << 11); }
        d.count -= TICKS_PER_SAMPLE;
        if (d.count <= 0) d.count += d.reload;
    }
    if (dsp_log && sys.frame >= (uint64_t)dsp_log) fprintf(stderr, "[dac] %d %d\n", (int16_t)d.mem[0x3FE], (int16_t)d.mem[0x3FF]);
    ring_put((int16_t)d.mem[0x3FE], (int16_t)d.mem[0x3FF]);
}

void dsp_reset(void) {
    memset(&d, 0, sizeof d);
    for (int i = 0; i < 1024; i++) d.code[i] = 0x8380;          /* sleep */
    d.reload = d.count = TICKS_PER_SAMPLE;
    d.opmask = 0xFF;
    d.noise = 0x1234;
}

/* ------------------------------------------------------------ ARM side -- */
static void ei_write(uint32_t a, uint16_t v) {
    a &= 0xFF;
    if (a >= 0x70 && a <= 0x7C) { d.supply[a - 0x70] = 1; d.mem[a] = v; }
    else if (!(a & 0x80)) d.mem[a & 0x7F] = v;
}

static uint16_t eo_read(uint32_t a) {
    a = 0x300 + (a & 0xFF);
    switch (a) {
    case 0x3EB: return d.audio_status;
    case 0x3EC: return d.sema_status;
    case 0x3ED: return d.sema_data;
    case 0x3EE: return d.int_word;
    case 0x3EF: return (uint16_t)d.reload;
    }
    return d.mem[a];
}

int dsp_clio_write(uint32_t off, uint32_t v) {
    if (off >= 0x1800 && off < 0x2000) {                        /* code, two words per write */
        uint32_t a = ((off & ~0x400u) - 0x1800) >> 1;
        d.code[a & 0x3FF] = (uint16_t)(v >> 16);
        d.code[(a + 1) & 0x3FF] = (uint16_t)v;
        return 1;
    }
    if (off >= 0x2000 && off < 0x3000) {                        /* code, one word per write */
        d.code[(((off & ~0x800u) - 0x2000) >> 2) & 0x3FF] = (uint16_t)v;
        return 1;
    }
    if (off >= 0x3000 && off < 0x3400) {
        uint32_t a = (off - 0x3000) >> 1;
        ei_write(a, (uint16_t)(v >> 16));
        ei_write(a + 1, (uint16_t)v);
        return 1;
    }
    if (off >= 0x3400 && off < 0x3800) { ei_write((off - 0x3400) >> 2, (uint16_t)v); return 1; }
    switch (off) {
    case 0x17D0: d.sema_data = (uint16_t)v; d.sema_status = 8; return 1;   /* ARM posts a value */
    case 0x17E8: d.pc = 0; d.rbase = 0; d.rmap = 0; d.opmask = 0xFF; return 1;
    case 0x17FC: d.running = v != 0; return 1;
    }
    return 0;
}

int dsp_clio_read(uint32_t off, uint32_t *v) {
    if (off >= 0x3800 && off < 0x3C00) {
        uint32_t a = (off - 0x3800) >> 1;
        *v = (uint32_t)eo_read(a) << 16 | eo_read(a + 1);
        return 1;
    }
    if (off >= 0x3C00 && off < 0x4000) { *v = eo_read((off - 0x3C00) >> 2); return 1; }
    switch (off) {
    case 0x17D0: *v = (uint32_t)d.sema_status << 16 | d.sema_data; return 1;
    case 0x17F0: *v = (uint32_t)noise() << 16 | noise(); return 1;
    case 0x17FC: *v = d.running; return 1;
    }
    return 0;
}
