/* 3DODroid - CLIO: interrupt controller, timers, video line counters.
 * Register offsets follow the public Opera hardware notes; anything not yet
 * understood is stored and logged so BIOS bring-up can fill it in. */
#include "sys3do.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "xbus.h"

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
    uint32_t dma_en, expctl;
    uint32_t dipir_req[2];           /* 0x410/0x414: device that raised attention (0x4000|unit) */
    uint64_t dma_done_at;
    int dsp_acc;
} c;

static void dsp_dma_reset(void);

void clio_reset(void) {
    memset(&c, 0, sizeof c);
    dsp_dma_reset();
    c.slack = 0x150;
    c.seed = 0x12345678;
    c.reg[0x28 >> 2] = 0x01;          /* cstatbits: power-on reset */
}

void clio_raise(int word, uint32_t bits) {
    c.pend[word] |= bits;
    if (c.pend[1] & c.mask[1]) c.pend[0] |= 0x80000000u;   /* second word cascades into bit 31 */
}

/* XBUS interrupt (word 0 bit 2) is level-triggered: it follows the
 * devices' enabled status/data-ready state rather than latching. */
static void sync_levels(void) {
    if (xbus_irq_pending()) c.pend[0] |= 1u << 2; else c.pend[0] &= ~(1u << 2);
}

int clio_fiq_pending(void) {
    sync_levels();
    /* word 1 cascades into word 0 bit 31, which is always enabled in the
     * word 0 mask (the kernel never sets it but expects to see it) */
    int w1 = (c.pend[1] & c.mask[1]) != 0;
    if (w1) c.pend[0] |= 0x80000000u; else c.pend[0] &= ~0x80000000u;
    return ((c.pend[0] & (c.mask[0] | 0x80000000u))) != 0;
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
/* RAM -> DSP input DMA: 13 channels at MADAM 0x400 + 16*ch (addr, len-4,
 * next addr, next len-4), enabled through CLIO 0x300/0x304, completion on
 * CLIO word 0 bit 16+ch. With no DSP core, channels are drained at a fixed
 * rate: channel 0 (streamed stereo audio) at 4 bytes per 44.1kHz frame, the
 * others (sample playback) at 2. When a buffer runs out the queued next
 * buffer is loaded, or the channel stops. */
#define DSP_DMA_CHANNELS 13
static int64_t dsp_dma_acc[DSP_DMA_CHANNELS];
static uint32_t dsp_dma_on;

static void dsp_dma_reset(void) {
    memset(dsp_dma_acc, 0, sizeof dsp_dma_acc);
    dsp_dma_on = 0;
    dsp_dma_next_pending = 0;
}

static void dsp_dma_tick(int cycles) {
    for (int ch = 0; ch < DSP_DMA_CHANNELS; ch++) {
        if (!(dsp_dma_on & (1u << ch))) continue;
        uint32_t base = 0x400 + (uint32_t)ch * 16;
        dsp_dma_acc[ch] += (int64_t)cycles * 44100 * (ch == 0 ? 4 : 2);
        int64_t bytes = dsp_dma_acc[ch] / 12500000;
        if (!bytes) continue;
        dsp_dma_acc[ch] -= bytes * 12500000;
        int32_t left = (int32_t)madam_reg(base + 4) - (int32_t)bytes;
        madam_set_reg(base, madam_reg(base) + (uint32_t)bytes);
        if (left >= -4) { madam_set_reg(base + 4, (uint32_t)left); continue; }
        if (dsp_dma_next_pending & (1u << ch)) {
            dsp_dma_next_pending &= ~(1u << ch);
            madam_set_reg(base, madam_reg(base + 8));
            madam_set_reg(base + 4, madam_reg(base + 12));
        } else {
            madam_set_reg(base + 4, 0xFFFFFFFCu);
            dsp_dma_on &= ~(1u << ch);
        }
        clio_raise(0, 1u << (16 + ch));
    }
}

void clio_tick(int cycles) {
    dsp_dma_tick(cycles);
    /* bring-up HLE of the DSP's periodic interrupt (word 0 bit 11): the
     * audio folio's clock. 44100 Hz / 184 samples per tick at 12.5MHz CPU. */
    if (sys.dsp_tick_hle) {
        c.dsp_acc += cycles;
        const int per = 12500000 / (44100 / 184);
        while (c.dsp_acc >= per) { c.dsp_acc -= per; clio_raise(0, 1u << 11); }
    }
    if (c.dma_done_at && arm.cycles >= c.dma_done_at) {
        c.dma_done_at = 0;
        clio_raise(0, sys.xbus_dma_irq ? sys.xbus_dma_irq : (1u << 29));
    }
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
    if (off >= 0x500 && off < 0x600) return xbus_read(off & ~0x3Fu);
    switch (off) {
    case 0x00: return 0x02020000;               /* CLIO revision */
    case 0x08: return c.vint0;
    case 0x0C: return c.vint1;
    case 0x30: return 0;                        /* hcnt */
    case 0x34: return c.line | (c.field << 11); /* vcnt + field */
    case 0x3C: c.seed = c.seed * 1103515245u + 12345u; return c.seed;
    case 0x40: case 0x44: sync_levels(); clio_fiq_pending(); return c.pend[0];
    case 0x48: case 0x4C: return c.mask[0] | 0x80000000u;
    case 0x60: case 0x64: return c.pend[1];
    case 0x68: case 0x6C: return c.mask[1];
    case 0x200: case 0x204: return (uint32_t)c.timer_ctl;
    case 0x208: case 0x20C: return (uint32_t)(c.timer_ctl >> 32);
    case 0x220: return c.slack;
    case 0x28: return c.reg[0x28 >> 2];
    case 0x400: case 0x404: return c.expctl | 0x80;   /* expctl: bit 7 = bus ready */
    case 0x300: case 0x304: case 0x308: return c.dma_en | dsp_dma_on;
    case 0x410: return c.dipir_req[0];
    case 0x414: return c.dipir_req[1];
    }
    if (off >= 0x100 && off < 0x180) {
        int t = (off - 0x100) >> 3;
        return (off & 4) ? c.timer_bak[t] : c.timer_cnt[t];
    }
    if (off == 0x3FB8 && sys.dsp_tick_hle)       /* HLE: audio kernel's 44.1kHz frame counter */
        return (uint32_t)((arm.cycles * 44100ull / 12500000ull) & 0xFFFF);
    if (sys.log_dsp && off >= 0x1700 && off < 0x4000)
        fprintf(stderr, "[dsp] rd %04x = %08x pc=%08x f%llu\n", off, c.reg[(off >> 2) & 0x3FFF], arm.cur_pc, (unsigned long long)sys.frame);
    return c.reg[(off >> 2) & 0x3FFF];
}

void clio_write(uint32_t off, uint32_t v) {
    if (sys.log_dsp && off >= 0x1700 && off < 0x4000 && !(off >= 0x1800 && off < 0x2000))
        fprintf(stderr, "[dsp] wr %04x = %08x pc=%08x f%llu\n", off, v, arm.cur_pc, (unsigned long long)sys.frame);
    if (off >= 0x500 && off < 0x600) { xbus_write(off & ~0x3Fu, v); return; }
    c.reg[(off >> 2) & 0x3FFF] = v;
    switch (off) {
    case 0x08: c.vint0 = v; return;
    case 0x0C: c.vint1 = v; return;
    case 0x40: clio_raise(0, v); return;
    case 0x44: c.pend[0] &= ~v; return;
    case 0x48:
        if (sys.irq_watch && (v & sys.irq_watch)) sys_backtrace("mask0 set");
        if (sys.log_irq) fprintf(stderr, "[irq] mask0 |= %08x pc=%08x\n", v, arm.cur_pc);
        c.mask[0] |= v;
        return;
    case 0x4C:
        if (sys.irq_watch && (v & sys.irq_watch)) sys_backtrace("mask0 clear");
        if (sys.log_irq) fprintf(stderr, "[irq] mask0 &= ~%08x pc=%08x\n", v, arm.cur_pc);
        c.mask[0] &= ~v;
        return;
    case 0x60: clio_raise(1, v); return;
    case 0x64: c.pend[1] &= ~v; if (!(c.pend[1] & c.mask[1])) c.pend[0] &= ~0x80000000u; return;
    case 0x68: if (sys.log_irq) fprintf(stderr, "[irq] mask1 |= %08x pc=%08x\n", v, arm.cur_pc); c.mask[1] |= v; return;
    case 0x6C: if (sys.log_irq) fprintf(stderr, "[irq] mask1 &= ~%08x pc=%08x\n", v, arm.cur_pc); c.mask[1] &= ~v; return;
    case 0x200: c.timer_ctl |= v; return;
    case 0x204: c.timer_ctl &= ~(uint64_t)v; return;
    case 0x208: c.timer_ctl |= (uint64_t)v << 32; return;
    case 0x20C: c.timer_ctl &= ~((uint64_t)v << 32); return;
    case 0x220: c.slack = v & 0x7FF; return;
    case 0x300:                                  /* DSP input DMA enables (set) */
        dsp_dma_on |= v & ((1u << DSP_DMA_CHANNELS) - 1);
        return;
    case 0x304:                                  /* DMA enable set */
        c.dma_en |= v;
        dsp_dma_on |= v & ((1u << DSP_DMA_CHANNELS) - 1);
        if ((v & 0x00100000) && (c.reg[0x400 >> 2] & 0x800)) {   /* XBUS -> RAM */
            uint32_t dest = madam_reg(0x540), cnt = madam_reg(0x544) + 4;
            xbus_dma_from_device(dest, cnt);
            madam_set_reg(0x540, dest + cnt);
            madam_set_reg(0x544, 0xFFFFFFFCu);
            c.dma_en &= ~0x00100000u;
            c.dma_done_at = arm.cycles + cnt / 4 + 50;          /* completion IRQ shortly after */
        }
        return;
    case 0x308: c.dma_en &= ~v; dsp_dma_on &= ~v; return;
    case 0x17D0:                                 /* CPU -> DSP semaphore */
        /* bring-up HLE: no DSP core yet, so acknowledge at once (bit 18)
         * and echo the value as the DSP's reply */
        c.reg[0x17D0 >> 2] = 0x40000u | (v & 0xFFFF);
        return;
    case 0x410: c.dipir_req[0] = 0; return;
    case 0x414: c.dipir_req[1] = 0; return;
    case 0x400: c.reg[0x400 >> 2] = c.expctl |= v; return;
    case 0x404: c.expctl &= ~v; c.reg[0x400 >> 2] = c.expctl; return;
    case 0x28:
        /* Writing 0x30 here (after acknowledging a device's attention) makes
         * the hardware re-enter the ROM with status 0x40: the ROM runs the
         * dipir for the device latched in 0x410/0x414, then returns to the
         * caller with subs pc, lr, #8. Interrupts stay off meanwhile. */
        if (v & 0x30) {
            if (sys.log_irq) fprintf(stderr, "[clio] dipir trap from pc=%08x\n", arm.cur_pc);
            c.reg[0x28 >> 2] = 0x40;
            sys.rom_overlay = 1;
            arm.r[14] = arm.cur_pc + 4 + 8;               /* ROM returns with subs pc, lr, #8 */
            arm.r[15] = 0;
            arm.i = 1; arm.f = 1;                        /* dipir runs with interrupts off */
            arm.trap_taken = 1;
            return;
        }
        c.reg[0x28 >> 2] = 0;
        return;
    }
    if (off >= 0x100 && off < 0x180) {
        int t = (off - 0x100) >> 3;
        if (off & 4) c.timer_bak[t] = v & 0xFFFF; else c.timer_cnt[t] = v & 0xFFFF;
        return;
    }
    if (sys.log_unknown && sys.unk_logged < sys.log_cap) {
        sys.unk_logged++;
        fprintf(stderr, "[clio] wr %03x = %08x pc=%08x\n", off, v, arm.cur_pc);
    }
}

/* An XBUS device raising attention (e.g. disc inserted) latches its unit
 * number for the ROM dipir, which reads it from 0x410/0x414. */
void clio_dipir_request(int unit) {
    if (!(c.dipir_req[0] & 0x4000)) c.dipir_req[0] = 0x4000u | (uint32_t)unit;
    else if (!(c.dipir_req[1] & 0x4000)) c.dipir_req[1] = 0x4000u | (uint32_t)unit;
}
