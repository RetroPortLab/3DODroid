/* 3DODroid - MADAM: memory control, VDL/display, cel engine, matrix engine.
 * Bring-up skeleton: registers are shadowed and logged; the cel engine and
 * display list walker are filled in as the BIOS exercises them. */
#include "sys3do.h"
#include <string.h>
#include <stdio.h>

static struct {
    uint32_t reg[0x800 / 4];
    uint32_t vdl_addr;              /* current display list */
} m;

void madam_reset(void) { memset(&m, 0, sizeof m); }

uint32_t madam_read(uint32_t off) {
    switch (off) {
    case 0x000: return 0x01020000;  /* MADAM revision (production Opera) */
    case 0x004: return 0x00000029;  /* msysbits: 2MB DRAM + 1MB VRAM config */
    }
    if (off < 0x800) return m.reg[off >> 2];
    if (sys.log_unknown && sys.unk_logged < 400) {
        sys.unk_logged++;
        fprintf(stderr, "[madam] rd %03x pc=%08x\n", off, arm.cur_pc);
    }
    return 0;
}

void madam_write(uint32_t off, uint32_t v) {
    if (off < 0x800) m.reg[off >> 2] = v;
    if (off == 0x580) { m.vdl_addr = v; return; }
    if (sys.log_unknown && sys.unk_logged < 400) {
        sys.unk_logged++;
        fprintf(stderr, "[madam] wr %03x = %08x pc=%08x\n", off, v, arm.cur_pc);
    }
}

/* Display output: placeholder until the VDL walker is written.
 * Shows the first 320x240 16-bit framebuffer at the start of VRAM using the
 * 3DO's line-pair layout (two lines interleaved per 32-bit word). */
void video_scanline(int line) {
    uint32_t base = 0x200000;
    for (int x = 0; x < 320; x++) {
        uint32_t a = base + ((line >> 1) * 320 + x) * 4 + ((line & 1) ? 2 : 0);
        uint16_t p = mem_rd16(a);
        uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
        sys.fb[line * 320 + x] = (r << 19) | (g << 11) | (b << 3);
    }
}
