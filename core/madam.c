/* 3DODroid - MADAM: memory control, VDL/display, cel engine, matrix engine.
 * Bring-up skeleton: registers are shadowed and logged; the cel engine and
 * display list walker are filled in as the BIOS exercises them. */
#include "sys3do.h"
#include <string.h>
#include <stdio.h>
#include "cel.h"

static struct {
    uint32_t reg[0x800 / 4];
    uint32_t vdl_addr;              /* current display list */
} m;

void madam_reset(void) { memset(&m, 0, sizeof m); }
uint32_t madam_reg(uint32_t off) { return m.reg[(off >> 2) & 0x1FF]; }
void madam_set_reg(uint32_t off, uint32_t v) { m.reg[(off >> 2) & 0x1FF] = v; }

uint32_t madam_read(uint32_t off) {
    switch (off) {
    case 0x000: return 0x01020000;  /* MADAM revision (production Opera) */
    case 0x004: return 0x00000029;  /* msysbits: 2MB DRAM + 1MB VRAM config */
    }
    if (off < 0x800) return m.reg[off >> 2];
    if (sys.log_unknown && sys.unk_logged < sys.log_cap) {
        sys.unk_logged++;
        fprintf(stderr, "[madam] rd %03x pc=%08x\n", off, arm.cur_pc);
    }
    return 0;
}

uint32_t madam_shown_fb;
uint32_t dsp_dma_next_pending;      /* per RAM->DSP channel: next addr/len written */
void madam_fallback_frame(void);

/* Control port: daisy-chained pods shift their data in each field. A
 * standard control pad sends 16 bits: ID 100, then Down Up Right Left A B C
 * P X RShift LShift (1 = pressed), then 00. Zeros after the last pod end
 * the chain. */
uint32_t pad_buttons[2];                    /* event.h layout: ControlDown = bit 31 */
int pad_count = 1;

static void pbus_transfer(uint32_t in_addr, uint32_t out_addr, uint32_t len) {
    (void)out_addr;
    for (uint32_t i = 0; i < len; i += 4) mem_wr32(in_addr + i, 0);
    uint32_t pos = 0;
    for (int p = 0; p < pad_count && p < 2; p++) {
        uint16_t w = (uint16_t)(0x8000u | ((pad_buttons[p] >> 19) & 0x1FFCu));
        uint32_t a = in_addr + pos, sh = (a & 2) ? 0 : 16;
        uint32_t word = mem_rd32(a & ~3u);
        word = (word & ~(0xFFFFu << sh)) | ((uint32_t)w << sh);
        mem_wr32(a & ~3u, word);
        pos += 2;
    }
}      /* bring-up: frame buffer last drawn by the cel engine */

void madam_write(uint32_t off, uint32_t v) {
    if (off < 0x800) m.reg[off >> 2] = v;
    if (off == 0x580) { m.vdl_addr = v; if (sys.log_unknown) fprintf(stderr, "[madam] VDL head = %08x\n", v); return; }
    if (off >= 0x400 && off < 0x4D0 && ((off & 0xF) == 0xC)) {   /* next length: next buffer queued */
        dsp_dma_next_pending |= 1u << ((off - 0x400) >> 4);
        return;
    }
    switch (off) {
    case 0x008:                                   /* MCTL */
        if (v & 0x8000) {                         /* player bus (control port) DMA */
            pbus_transfer(m.reg[0x570 >> 2], m.reg[0x578 >> 2], m.reg[0x574 >> 2] + 4);
            m.reg[0x008 >> 2] &= ~0x8000u;
        }
        return;
    case 0x100:                                   /* cel engine start */
        cel_set_regs(m.reg[0x130 >> 2], m.reg[0x134 >> 2], m.reg[0x138 >> 2], m.reg[0x13C >> 2]);
        cel.first_ccb = m.reg[0x5A4 >> 2];
        cel_run();
        madam_shown_fb = m.reg[0x13C >> 2];
        return;
    case 0x110: case 0x130: case 0x134: case 0x138: case 0x13C: case 0x5A4:
        return;                                   /* latched, used at start */
    }
    if (sys.log_unknown && sys.unk_logged < sys.log_cap) {
        sys.unk_logged++;
        fprintf(stderr, "[madam] wr %03x = %08x pc=%08x\n", off, v, arm.cur_pc);
    }
}

/* ---- Display: VDL (video display list) walker ----
 * The head comes from MADAM 0x580. Each entry:
 *   word0 DMA control: bit21 ENVIDDMA, bit16 LDCUR, bit15 LDPREV,
 *         bits14-9 number of colour/control words, bits8-0 line count
 *   word1 current frame buffer, word2 previous frame buffer, word3 next entry
 *   then the colour/control words:
 *     bit31 clear: CLUT write - pen index bits28-24, RGB in bits23-0,
 *                  bits30-29 select R/G/B or all three
 *     0xC0000000 prefix: display control (background, CLUT bypass, ...)
 *     0xE0000000 prefix: background colour
 * Frame buffers use the line-pair layout, 320 pixels per line. */
static uint8_t clut_r[32], clut_g[32], clut_b[32];
static uint32_t bg_color;

static void clut_reset(void) {
    for (int i = 0; i < 32; i++) clut_r[i] = clut_g[i] = clut_b[i] = (uint8_t)((i << 3) | (i >> 2));
}

static void vdl_colour_word(uint32_t w) {
    if ((w & 0xE0000000u) == 0xE0000000u) { bg_color = w & 0xFFFFFF; return; }
    if ((w & 0xC0000000u) == 0xC0000000u) return;           /* display control: not modelled yet */
    if (w & 0x80000000u) return;                            /* AMY control */
    int pen = (w >> 24) & 31, sel = (w >> 29) & 3;
    uint8_t r = (uint8_t)(w >> 16), g = (uint8_t)(w >> 8), b = (uint8_t)w;
    if (sel == 0 || sel == 3) clut_r[pen] = r;
    if (sel == 0 || sel == 2) clut_g[pen] = g;
    if (sel == 0 || sel == 1) clut_b[pen] = b;
}

/* The list starts at the top of the field: the first VBLANK_LINES lines are
 * vertical blanking, then the visible 240 lines follow. */
#define VBLANK_LINES 16

void video_render_frame(void) {
    uint32_t vdl = m.reg[0x580 >> 2];
    if (!vdl) { madam_fallback_frame(); return; }
    clut_reset();
    bg_color = 0;
    uint32_t cur = 0x200000;
    int vline = 0, guard = 0;
    while (vline < VBLANK_LINES + 240 && guard++ < 256 && vdl && vdl < RAM_SIZE) {
        uint32_t ctl = mem_rd32(vdl), next = mem_rd32(vdl + 12);
        int nwords = (ctl >> 9) & 0x3F, nlines = ctl & 0x1FF;
        if (ctl & 0x10000u) cur = mem_rd32(vdl + 4);       /* LDCUR */
        int dma_on = (ctl & 0x200000u) != 0;
        for (int i = 0; i < nwords; i++) vdl_colour_word(mem_rd32(vdl + 16 + i * 4));
        for (int n = 0; n < nlines; n++, vline++) {
            int line = vline - VBLANK_LINES;
            uint32_t row = cur + (uint32_t)(n >> 1) * 320 * 4 + ((n & 1) ? 2 : 0);
            if (line < 0 || line >= 240) continue;
            uint32_t *dst = &sys.fb[line * 320];
            if (!dma_on) { for (int x = 0; x < 320; x++) dst[x] = bg_color; continue; }
            for (int x = 0; x < 320; x++) {
                uint16_t p = mem_rd16(row + (uint32_t)x * 4);
                uint32_t r = clut_r[(p >> 10) & 31], g = clut_g[(p >> 5) & 31], b = clut_b[p & 31];
                dst[x] = (r << 16) | (g << 8) | b;
            }
        }
        cur += (uint32_t)((nlines + 1) >> 1) * 320 * 4;
        if (next == vdl) break;
        vdl = next;
    }
}

/* Before any VDL exists (early BIOS), show the frame buffer the cel engine
 * last drew into. */
void madam_fallback_frame(void) {
    uint32_t base = madam_shown_fb ? madam_shown_fb : 0x200000;
    for (int line = 0; line < 240; line++)
        for (int x = 0; x < 320; x++) {
            uint32_t a = base + ((line >> 1) * 320 + x) * 4 + ((line & 1) ? 2 : 0);
            uint16_t p = mem_rd16(a);
            uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
            sys.fb[line * 320 + x] = (r << 19) | (g << 11) | (b << 3);
        }
}

void video_scanline(int line) { (void)line; }
