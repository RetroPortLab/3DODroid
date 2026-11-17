/* 3DODroid - CLIO: interrupt controller, timers, video line counters.
 * Register offsets follow the public Opera hardware notes; anything not yet
 * understood is stored and logged so BIOS bring-up can fill it in. */
#include "sys3do.h"
#include <string.h>
#include <stdio.h>

static struct {
    uint32_t reg[0x10000 / 4];       /* shadow of everything written */
    uint32_t pend[2], mask[2];       /* interrupt words 0 and 1 */
    uint32_t vint0, vint1;           /* lines that raise vint0/vint1 */
    uint32_t line, field;
    uint32_t timer_cnt[16], timer_bak[16];
    uint64_t timer_ctl;              /* 4 control bits per timer */
    uint32_t slack;
    int timer_acc;
    uint32_t seed;
} c;

void clio_reset(void) {
    memset(&c, 0, sizeof c);
    c.slack = 0x150;
    c.seed = 0x12345678;
    c.reg[0x28 >> 2] = 0x01;          /* cstatbits: power-on reset */
}

void clio_raise(int word, uint32_t bits) {
    c.pend[word] |= bits;
    if (c.pend[1] & c.mask[1]) c.pend[0] |= 0x80000000u;   /* second word cascades into bit 31 */
}

int clio_fiq_pending(void) {
    if (c.pend[1] & c.mask[1]) c.pend[0] |= 0x80000000u;
    return (c.pend[0] & c.mask[0]) != 0;
}

void clio_set_line(int line) {
    c.line = (uint32_t)line;
    if (line == 0) c.field ^= 1;
    if ((uint32_t)line == (c.vint0 & 0x7FF)) clio_raise(0, 1u << 0);
    if ((uint32_t)line == (c.vint1 & 0x7FF)) clio_raise(0, 1u << 1);
}

/* 16 down-counters. Each control nibble: bit0 decrement, bit1 reload,
 * bit2 cascade from previous timer, bit3 interrupt on underflow (odd timers).
 * The prescaler ("slack") sets how many CPU cycles make one timer tick. */
void clio_tick(int cycles) {
    c.timer_acc += cycles;
    int per = (int)(c.slack ? c.slack : 0x150) / 4 + 1;   /* approximation, refined during bring-up */
    while (c.timer_acc >= per) {
        c.timer_acc -= per;
        int carry = 0;
        for (int t = 0; t < 16; t++) {
            uint32_t ctl = (uint32_t)(c.timer_ctl >> (t * 4)) & 15;
            int dec = (ctl & 1) && (!(ctl & 4) || carry);
            carry = 0;
            if (!dec) continue;
            if (c.timer_cnt[t]-- == 0) {
                carry = 1;
                if (ctl & 2) c.timer_cnt[t] = c.timer_bak[t];
                else { c.timer_ctl &= ~(1ull << (t * 4)); c.timer_cnt[t] = 0xFFFF; }
                /* timers 15,13,...,1 map to interrupt bits 3..10 */
                if (t & 1) clio_raise(0, 1u << (3 + (15 - t) / 2));
            }
            c.timer_cnt[t] &= 0xFFFF;
        }
    }
}

uint32_t clio_read(uint32_t off) {
    switch (off) {
    case 0x00: return 0x02020000;               /* CLIO revision */
    case 0x08: return c.vint0;
    case 0x0C: return c.vint1;
    case 0x30: return 0;                        /* hcnt */
    case 0x34: return c.line | (c.field << 11); /* vcnt + field */
    case 0x3C: c.seed = c.seed * 1103515245u + 12345u; return c.seed;
    case 0x40: case 0x44: return c.pend[0];
    case 0x48: case 0x4C: return c.mask[0];
    case 0x60: case 0x64: return c.pend[1];
    case 0x68: case 0x6C: return c.mask[1];
    case 0x200: case 0x204: return (uint32_t)c.timer_ctl;
    case 0x208: case 0x20C: return (uint32_t)(c.timer_ctl >> 32);
    case 0x220: return c.slack;
    case 0x28: return c.reg[0x28 >> 2];
    }
    if (off >= 0x100 && off < 0x180) {
        int t = (off - 0x100) >> 3;
        return (off & 4) ? c.timer_bak[t] : c.timer_cnt[t];
    }
    return c.reg[(off >> 2) & 0x3FFF];
}

void clio_write(uint32_t off, uint32_t v) {
    c.reg[(off >> 2) & 0x3FFF] = v;
    switch (off) {
    case 0x08: c.vint0 = v; return;
    case 0x0C: c.vint1 = v; return;
    case 0x40: clio_raise(0, v); return;
    case 0x44: c.pend[0] &= ~v; return;
    case 0x48: c.mask[0] |= v; return;
    case 0x4C: c.mask[0] &= ~v; return;
    case 0x60: clio_raise(1, v); return;
    case 0x64: c.pend[1] &= ~v; if (!(c.pend[1] & c.mask[1])) c.pend[0] &= ~0x80000000u; return;
    case 0x68: c.mask[1] |= v; return;
    case 0x6C: c.mask[1] &= ~v; return;
    case 0x200: c.timer_ctl |= v; return;
    case 0x204: c.timer_ctl &= ~(uint64_t)v; return;
    case 0x208: c.timer_ctl |= (uint64_t)v << 32; return;
    case 0x20C: c.timer_ctl &= ~((uint64_t)v << 32); return;
    case 0x220: c.slack = v & 0x7FF; return;
    case 0x28: return;                       /* write clears status (stored above) */
    }
    if (off >= 0x100 && off < 0x180) {
        int t = (off - 0x100) >> 3;
        if (off & 4) c.timer_bak[t] = v & 0xFFFF; else c.timer_cnt[t] = v & 0xFFFF;
        return;
    }
    if (sys.log_unknown && sys.unk_logged < 400) {
        sys.unk_logged++;
        fprintf(stderr, "[clio] wr %03x = %08x pc=%08x\n", off, v, arm.cur_pc);
    }
}
