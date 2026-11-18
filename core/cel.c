/* 3DODroid - MADAM cel engine.
 *
 * Walks a CCB list starting at the address in MADAM 0x5A4 when the host
 * writes 0x100 (start), drawing each cel into the frame buffer described by
 * REGCTL0-3 (0x130-0x13C).
 *
 * CCB words are fetched in order, each only if its load flag is set:
 *   flags, next, source, plut, xpos, ypos,
 *   [LDSIZE] hdx, hdy, vdx, vdy   [LDPRS] hddx, hddy   [LDPPMP] pixc,
 *   [CCBPRE] pre0, pre1 (pre1 only for unpacked cels)
 * Relative pointers are relative to the address of the word after the field.
 *
 * Projection: row v starts at (X,Y) + v*(VDX,VDY); along a row the step is
 * (HDX,HDY) + v*(HDDX,HDDY). Each source pixel covers the quad between its
 * corner and the neighbouring corners. Positions are 16.16, HDX/HDY/HDDX/HDDY
 * are 12.20.
 *
 * Frame buffers use the 3DO line-pair layout: each 32-bit word holds pixel
 * (x, 2n) in its upper half and (x, 2n+1) in its lower half. */
#include "sys3do.h"
#include "cel.h"
#include "dbglog.h"
#include <string.h>
#include <stdio.h>

#define F_SKIP    0x80000000u
#define F_LAST    0x40000000u
#define F_NPABS   0x20000000u
#define F_SPABS   0x10000000u
#define F_PPABS   0x08000000u
#define F_LDSIZE  0x04000000u
#define F_LDPRS   0x02000000u
#define F_LDPPMP  0x01000000u
#define F_LDPLUT  0x00800000u
#define F_CCBPRE  0x00400000u
#define F_YOXY    0x00200000u
#define F_ACW     0x00040000u
#define F_ACCW    0x00020000u
#define F_TWD     0x00010000u
#define F_PXOR    0x00000800u
#define F_USEAV   0x00000400u
#define F_PACKED  0x00000200u
#define F_POVER   0x00000180u
#define F_PLUTPOS 0x00000040u
#define F_BGND    0x00000020u
#define F_NOBLK   0x00000010u
#define F_PLUTA   0x0000000Fu

CelState cel;

/* ---- frame buffer access ---- */
static inline uint32_t fb_addr(int x, int y) {
    return cel.wbase + (uint32_t)(y >> 1) * cel.fb_stride + (uint32_t)x * 4 + ((y & 1) ? 2 : 0);
}
static inline uint16_t fb_get(int x, int y) { return mem_rd16(fb_addr(x, y)); }
static inline void fb_put(int x, int y, uint16_t v) { mem_wr16(fb_addr(x, y), v); }

/* ---- source bit reader (big-endian bit order) ---- */
typedef struct { uint32_t addr; uint32_t bitpos; } BitRd;
static inline uint32_t bits(BitRd *b, int n) {
    uint32_t v = 0;
    while (n > 0) {
        uint32_t w = mem_rd32(b->addr + ((b->bitpos >> 5) << 2));
        int off = b->bitpos & 31, take = 32 - off;
        if (take > n) take = n;
        uint32_t chunk = (w << off) >> (32 - take);
        v = (v << take) | chunk;
        b->bitpos += take; n -= take;
    }
    return v;
}

static const int bpp_tab[8] = { 0, 1, 2, 4, 6, 8, 16, 0 };

typedef struct {
    uint32_t flags, plut, pixc, pre0, pre1;
    int bpp, linear, packed;
    uint32_t amv;     /* per-channel multipliers (3 bits each, R:G:B) from coded pixels */
} Ctx;

#define AMV_ONE 0x49  /* multiplier field 1 in each channel (FreeDO's default) */

/* decode one raw source pixel into 15-bit RGB + P bit; returns -1 if transparent */
static int decode(Ctx *c, uint32_t raw, uint16_t *out, int *pbit) {
    uint16_t col;
    c->amv = AMV_ONE;
    if (c->bpp == 16) {
        if (c->linear) col = (uint16_t)raw;
        else {                                  /* coded 16bpp: 5-bit index, 3-bit R/G/B multipliers, P bit */
            col = (uint16_t)((mem_rd16(c->plut + (raw & 31) * 2) & 0x7FFF) | (raw & 0x8000));
            c->amv = (raw >> 5) & 0x1FF;
        }
    } else if (c->bpp == 8 && c->linear) {
        /* uncoded 8bpp: RRRGGGBB expanded to 5:5:5 */
        uint32_t r = (raw >> 5) & 7, g = (raw >> 2) & 7, b = raw & 3;
        col = (uint16_t)(((r << 2 | r >> 1) << 10) | ((g << 2 | g >> 1) << 5) | (b << 3 | b << 1 | b >> 1));
    } else {
        uint32_t idx;
        if (c->bpp == 8) {                      /* coded 8bpp: 5-bit index, 3-bit multiplier for all channels */
            uint32_t m = (raw >> 5) & 7;
            idx = raw & 31;
            c->amv = m << 6 | m << 3 | m;
        } else if (c->bpp == 6) idx = raw & 31;
        else {
            /* 1/2/4 bpp: low bits come from the pixel, upper from PLUTA */
            uint32_t mask = (1u << c->bpp) - 1;
            idx = ((c->flags & F_PLUTA) << 1) & ~mask;
            idx = (idx | (raw & mask)) & 31;
        }
        col = mem_rd16(c->plut + idx * 2);
        if (c->bpp == 6) col = (uint16_t)((col & 0x7FFF) | ((raw & 0x20) ? 0x8000 : 0));
    }
    *pbit = col >> 15;
    *out = col & 0x7FFF;
    if (!(c->flags & F_BGND) && *out == 0) return -1;
    return 0;
}

/* ---- pixel processor ----
 * Behaviour as described by the FreeDO project (see README credits).
 * PIXC half (chosen by the P bit or the CCB's POVER override):
 *   15 1S   first source: 0 cel pixel, 1 frame buffer pixel
 *   14-13 MS multiplier: 0 MF field, 1 AMV from the pixel, 2 cel colour
 *            (multiplier and divider per channel), 3 cel colour (multiplier only)
 *   12-10 MF multiplier - 1      9-8 DF divider: 16, 2, 4, 8
 *   7-6 2S  second source: 0, AV, frame buffer, cel      5-1 AV   0 2D: halve the sum
 * With USEAV in the CCB, AV also controls: bit0 subtract, bit1 sign-extend,
 * bit2 no clipping, bits 3-4 shift the second source right. */
static const uint8_t df_shift[4] = { 4, 1, 2, 3 };

static inline uint16_t pixproc(Ctx *c, uint16_t src, int pbit, uint16_t dst) {
    uint32_t pov = (c->flags & F_POVER) >> 7;
    int sel = pov == 2 ? 0 : pov == 3 ? 1 : pbit;
    uint32_t p = sel ? (c->pixc >> 16) : (c->pixc & 0xFFFF);
    if (p == 0x1F00 && (src || (c->flags & F_NOBLK))) return src;   /* the common opaque case */
    int s1 = (p >> 15) & 1, ms = (p >> 13) & 3, mf = (p >> 10) & 7, df = (p >> 8) & 3;
    int s2 = (p >> 6) & 3, av = (p >> 1) & 31, div2 = p & 1;
    int neg = 0, xtend = 0, noclip = 0, dv3 = 0;
    if (c->flags & F_USEAV) { neg = av & 1; xtend = (av >> 1) & 1; noclip = (av >> 2) & 1; dv3 = (av >> 3) & 3; }
    int pxor = (c->flags & F_PXOR) != 0;
    uint16_t in1 = s1 ? dst : src;
    uint32_t out = 0;
    for (int sh = 10; sh >= 0; sh -= 5) {                 /* R, G, B */
        int ch = (sh == 10) ? 6 : (sh == 5) ? 3 : 0;      /* AMV field for this channel */
        int a = (in1 >> sh) & 31, cs = (src >> sh) & 31, m, d;
        switch (ms) {
        case 0:  m = mf; d = df_shift[df]; break;
        case 1:  m = (int)(c->amv >> ch) & 7; d = df_shift[df]; break;
        case 2:  m = cs >> 2; d = df_shift[cs & 3]; break;
        default: m = cs >> 2; d = df_shift[df]; break;
        }
        int c1 = (a * (m + 1)) >> d, c2;
        switch (s2) {
        case 0:  c2 = 0; break;
        case 1:  c2 = av >> dv3; break;
        case 2:  c2 = ((dst >> sh) & 31) >> dv3; break;
        default: c2 = cs >> dv3; break;
        }
        int aop, bop;
        if (pxor) { aop = 0; c1 &= 31; bop = neg ? (~c2 & 0xFF) : (c2 ^ c1); }
        else      { aop = c1; bop = neg ? (~c2 & 0xFF) : c2; }
        if (bop & 0x80) bop -= 256;                       /* 8-bit signed lanes */
        if (xtend) bop = (bop & 16) ? (bop | ~31) : (bop & 31);
        int v = (aop + bop + neg) >> div2;
        if (!noclip) v = v < 0 ? 0 : v > 31 ? 31 : v;
        out |= (uint32_t)(v & 31) << sh;
    }
    if (!(c->flags & F_NOBLK) && out == 0) out = 0x0400;  /* black would be transparent: use near-black */
    return (uint16_t)out;
}

/* ---- quad fill (one source pixel), coordinates in 16.16 ---- */
static void plot(Ctx *c, uint16_t col, int pbit, int x, int y) {
    if (x < cel.clip_x0 || y < cel.clip_y0 || x > cel.clip_x1 || y > cel.clip_y1) return;
    uint16_t d = fb_get(x, y);
    fb_put(x, y, pixproc(c, col, pbit, d));
}

static void fill_quad(Ctx *c, uint16_t col, int pbit, const int32_t *qx, const int32_t *qy) {
    /* fast path: axis-aligned rectangle */
    if (qy[0] == qy[1] && qx[1] == qx[2] && qy[2] == qy[3] && qx[3] == qx[0]) {
        int x0 = (qx[0] + 0xFFFF) >> 16, x1 = (qx[1] + 0xFFFF) >> 16;
        int y0 = (qy[0] + 0xFFFF) >> 16, y1 = (qy[3] + 0xFFFF) >> 16;
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) plot(c, col, pbit, x, y);
        return;
    }
    /* general convex quad: scanline with edge crossings at pixel centres */
    int32_t miny = qy[0], maxy = qy[0];
    for (int i = 1; i < 4; i++) { if (qy[i] < miny) miny = qy[i]; if (qy[i] > maxy) maxy = qy[i]; }
    int ys = (miny + 0x7FFF) >> 16, ye = (maxy + 0x7FFF) >> 16;
    if (ys < cel.clip_y0) ys = cel.clip_y0;
    if (ye > cel.clip_y1 + 1) ye = cel.clip_y1 + 1;
    for (int y = ys; y < ye; y++) {
        int64_t sy = ((int64_t)y << 16) + 0x8000;
        int64_t xl = INT64_MAX, xr = INT64_MIN;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int64_t y0 = qy[i], y1 = qy[j];
            if ((sy < y0) == (sy < y1)) continue;
            int64_t x = qx[i] + (sy - y0) * (qx[j] - qx[i]) / (y1 - y0);
            if (x < xl) xl = x;
            if (x > xr) xr = x;
        }
        if (xl > xr) continue;
        int xs = (int)((xl + 0x7FFF) >> 16), xe = (int)((xr + 0x7FFF) >> 16);
        for (int x = xs; x < xe; x++) plot(c, col, pbit, x, y);
    }
}

/* ---- one cel ---- */
static void draw_cel(Ctx *c, uint32_t src, int32_t x0, int32_t y0) {
    int w, h;
    if (c->packed) { w = 2048; h = (int)((c->pre0 >> 6) & 0x3FF) + 1; }
    else { w = (int)(c->pre1 & 0x7FF) + 1; h = (int)((c->pre0 >> 6) & 0x3FF) + 1; }
    int skipx = (c->pre0 >> 24) & 15;
    uint32_t rowaddr = src;

    for (int v = 0; v < h; v++) {
        /* row geometry */
        int64_t rx = (int64_t)x0 + (int64_t)v * cel.vdx, ry = (int64_t)y0 + (int64_t)v * cel.vdy;
        int64_t dx = cel.hdx + (int64_t)v * cel.hddx, dy = cel.hdy + (int64_t)v * cel.hddy;     /* 12.20 */
        int64_t nrx = rx + cel.vdx, nry = ry + cel.vdy;
        int64_t ndx = dx + cel.hddx, ndy = dy + cel.hddy;
        BitRd br = { rowaddr, 0 };
        uint32_t next_row;
        int u = 0;
        if (c->packed) {
            int offbits = c->bpp >= 8 ? 10 : 8;
            uint32_t off = bits(&br, offbits == 10 ? 16 : 8);
            if (offbits == 10) off &= 0x3FF;
            next_row = rowaddr + (off + 2) * 4;
            int done = 0;
            while (!done && u < w) {
                uint32_t t = bits(&br, 2), cnt;
                switch (t) {
                case 0: done = 1; break;
                case 1: cnt = bits(&br, 6) + 1;
                    for (uint32_t k = 0; k < cnt; k++, u++) {
                        uint32_t raw = bits(&br, c->bpp); uint16_t col; int pb;
                        if (u < skipx || decode(c, raw, &col, &pb) < 0) continue;
                        int32_t qx[4], qy[4];
                        qx[0] = (int32_t)(rx + ((dx * (u - skipx)) >> 4)); qy[0] = (int32_t)(ry + ((dy * (u - skipx)) >> 4));
                        qx[1] = (int32_t)(rx + ((dx * (u - skipx + 1)) >> 4)); qy[1] = (int32_t)(ry + ((dy * (u - skipx + 1)) >> 4));
                        qx[2] = (int32_t)(nrx + ((ndx * (u - skipx + 1)) >> 4)); qy[2] = (int32_t)(nry + ((ndy * (u - skipx + 1)) >> 4));
                        qx[3] = (int32_t)(nrx + ((ndx * (u - skipx)) >> 4)); qy[3] = (int32_t)(nry + ((ndy * (u - skipx)) >> 4));
                        fill_quad(c, col, pb, qx, qy);
                    }
                    break;
                case 2: cnt = bits(&br, 6) + 1; u += (int)cnt; break;
                case 3: { cnt = bits(&br, 6) + 1;
                    uint32_t raw = bits(&br, c->bpp); uint16_t col; int pb;
                    int vis = decode(c, raw, &col, &pb) >= 0;
                    for (uint32_t k = 0; k < cnt; k++, u++) {
                        if (!vis || u < skipx) continue;
                        int32_t qx[4], qy[4];
                        qx[0] = (int32_t)(rx + ((dx * (u - skipx)) >> 4)); qy[0] = (int32_t)(ry + ((dy * (u - skipx)) >> 4));
                        qx[1] = (int32_t)(rx + ((dx * (u - skipx + 1)) >> 4)); qy[1] = (int32_t)(ry + ((dy * (u - skipx + 1)) >> 4));
                        qx[2] = (int32_t)(nrx + ((ndx * (u - skipx + 1)) >> 4)); qy[2] = (int32_t)(nry + ((ndy * (u - skipx + 1)) >> 4));
                        qx[3] = (int32_t)(nrx + ((ndx * (u - skipx)) >> 4)); qy[3] = (int32_t)(nry + ((ndy * (u - skipx)) >> 4));
                        fill_quad(c, col, pb, qx, qy);
                    }
                    break; }
                }
            }
        } else {
            uint32_t woff = c->bpp >= 8 ? ((c->pre1 >> 16) & 0x3FF) : (c->pre1 >> 24);
            next_row = rowaddr + (woff + 2) * 4;
            for (u = 0; u < w; u++) {
                uint32_t raw = bits(&br, c->bpp); uint16_t col; int pb;
                if (u < skipx || decode(c, raw, &col, &pb) < 0) continue;
                int32_t qx[4], qy[4];
                qx[0] = (int32_t)(rx + ((dx * (u - skipx)) >> 4)); qy[0] = (int32_t)(ry + ((dy * (u - skipx)) >> 4));
                qx[1] = (int32_t)(rx + ((dx * (u - skipx + 1)) >> 4)); qy[1] = (int32_t)(ry + ((dy * (u - skipx + 1)) >> 4));
                qx[2] = (int32_t)(nrx + ((ndx * (u - skipx + 1)) >> 4)); qy[2] = (int32_t)(nry + ((ndy * (u - skipx + 1)) >> 4));
                qx[3] = (int32_t)(nrx + ((ndx * (u - skipx)) >> 4)); qy[3] = (int32_t)(nry + ((ndy * (u - skipx)) >> 4));
                fill_quad(c, col, pb, qx, qy);
            }
        }
        rowaddr = next_row;
    }
}

/* ---- CCB list walk ---- */
void cel_run(void) {
    uint32_t ccb = cel.first_ccb;
    int guard = 0;
    cel.fb_stride = cel.width_words_x2 ? cel.width_words_x2 : 320 * 4;
    while (ccb && ccb < RAM_SIZE && guard++ < 4096) {
        Ctx c; memset(&c, 0, sizeof c);
        uint32_t a = ccb;
        c.flags = mem_rd32(a); a += 4;
        uint32_t nextf = a, next = mem_rd32(a); a += 4;
        uint32_t srcf = a, src = mem_rd32(a); a += 4;
        uint32_t plutf = a, plut = mem_rd32(a); a += 4;
        int32_t x = (int32_t)mem_rd32(a); a += 4;
        int32_t y = (int32_t)mem_rd32(a); a += 4;
        if (c.flags & F_LDSIZE) { cel.hdx = (int32_t)mem_rd32(a); cel.hdy = (int32_t)mem_rd32(a + 4); cel.vdx = (int32_t)mem_rd32(a + 8); cel.vdy = (int32_t)mem_rd32(a + 12); a += 16; }
        if (c.flags & F_LDPRS) { cel.hddx = (int32_t)mem_rd32(a); cel.hddy = (int32_t)mem_rd32(a + 4); a += 8; }
        if (c.flags & F_LDPPMP) { cel.pixc = mem_rd32(a); a += 4; }
        c.pixc = cel.pixc;
        uint32_t nexta = (c.flags & F_NPABS) ? next : nextf + 4 + next;
        uint32_t srca = (c.flags & F_SPABS) ? src : srcf + 4 + src;
        if (c.flags & F_LDPLUT) cel.plut = (c.flags & F_PPABS) ? plut : plutf + 4 + plut;
        c.plut = cel.plut;
        c.packed = (c.flags & F_PACKED) != 0;
        if (c.flags & F_CCBPRE) {
            c.pre0 = mem_rd32(a); a += 4;
            if (!c.packed) { c.pre1 = mem_rd32(a); a += 4; }
        } else {
            c.pre0 = mem_rd32(srca); srca += 4;
            if (!c.packed) { c.pre1 = mem_rd32(srca); srca += 4; }
        }
        c.bpp = bpp_tab[c.pre0 & 7];
        c.linear = (c.pre0 & 0x10) != 0;
        {
            uint32_t key = (c.pre0 & 0x17) | (c.packed << 5) | ((c.flags & 0x1DF0) << 4);
            if (dbg_once(8, key ^ (c.pixc * 2654435761u)))
                dbg("cel: mode flags %08x pre0 %08x pre1 %08x pixc %08x", c.flags, c.pre0, c.pre1, c.pixc);
        }
        if (!(c.flags & F_SKIP) && c.bpp) {
            cel.cels_drawn++;
            draw_cel(&c, srca, x, y);
        }
        if (c.flags & F_LAST) break;
        if (c.flags & F_YOXY) { /* next cel's position follows: handled by its own CCB */ }
        ccb = nexta;
    }
}

void cel_set_regs(uint32_t regctl0, uint32_t regctl1, uint32_t rbase, uint32_t wbase) {
    /* REGCTL0 encodes the frame buffer width; the BIOS uses 0x1414 for 320.
     * Width in pixels: decode the two modulo fields when we see other values. */
    (void)regctl0;
    cel.width_words_x2 = 320 * 4;
    cel.clip_x0 = 0; cel.clip_y0 = 0;
    cel.clip_x1 = (int)(regctl1 & 0x7FF);
    cel.clip_y1 = (int)((regctl1 >> 16) & 0x7FF);
    cel.rbase = rbase; cel.wbase = wbase;
}
