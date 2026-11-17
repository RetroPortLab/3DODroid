/* 3DODroid - 3DO Opera memory map
 *   0x00000000-0x001FFFFF  DRAM 2MB
 *   0x00200000-0x002FFFFF  VRAM 1MB
 *   0x03000000-0x030FFFFF  BIOS ROM (overlaid at 0 after reset)
 *   0x03140000             NVRAM (32KB, one byte per word)
 *   0x03200000             SPORT (VRAM page flash/copy)
 *   0x03300000             MADAM
 *   0x03400000             CLIO
 */
#include "sys3do.h"
#include <string.h>
#include <stdio.h>

Sys3do sys;

static void unk(const char *what, uint32_t a, uint32_t v) {
    if (sys.log_unknown && sys.unk_logged < 400) {
        sys.unk_logged++;
        fprintf(stderr, "[bus] %s %08x = %08x  pc=%08x\n", what, a, v, arm.cur_pc);
    }
}

static inline uint32_t ram_rd(uint32_t a) { uint32_t v; memcpy(&v, sys.ram + a, 4); return v; }
static inline void ram_wr(uint32_t a, uint32_t v) { memcpy(sys.ram + a, &v, 4); }
/* RAM is stored host-little-endian word-wise; the 3DO is big-endian, so byte
 * accesses flip the low address bits. */
#define BYTE_SWZ(a) ((a) ^ 3)

uint32_t bus_read32(uint32_t a) {
    a &= ~3u;
    if (a < RAM_SIZE) {
        if (sys.rom_overlay) return sys.rom_w[(a & (ROM_SIZE - 1)) >> 2];
        return ram_rd(a);
    }
    if (a >= 0x03000000 && a < 0x03000000 + ROM_SIZE) return sys.rom_w[(a - 0x03000000) >> 2];
    if (a >= 0x03140000 && a < 0x03180000) return sys.nvram[((a - 0x03140000) >> 2) & (NVRAM_SIZE - 1)];
    if (a >= 0x03300000 && a < 0x03400000) return madam_read(a - 0x03300000);
    if (a >= 0x03400000 && a < 0x03500000) return clio_read(a - 0x03400000);
    unk("rd32", a, 0);
    return 0;
}

uint8_t bus_read8(uint32_t a) {
    uint32_t w = bus_read32(a & ~3u);
    return (uint8_t)(w >> ((3 - (a & 3)) * 8));
}

uint32_t bus_fetch(uint32_t a) {
    if (sys.rom_overlay && a >= 0x03000000) sys.rom_overlay = 0;   /* BIOS has jumped to its real address */
    if (a < RAM_SIZE && !sys.rom_overlay) return ram_rd(a);
    return bus_read32(a);
}

void bus_write32(uint32_t a, uint32_t v) {
    a &= ~3u;
    if (a < RAM_SIZE) { ram_wr(a, v); return; }
    if (a >= 0x03140000 && a < 0x03180000) { sys.nvram[((a - 0x03140000) >> 2) & (NVRAM_SIZE - 1)] = (uint8_t)v; sys.nvram_dirty = 1; return; }
    if (a >= 0x03200000 && a < 0x03300000) { sport_write(a - 0x03200000, v); return; }
    if (a >= 0x03300000 && a < 0x03400000) { madam_write(a - 0x03300000, v); return; }
    if (a >= 0x03400000 && a < 0x03500000) { clio_write(a - 0x03400000, v); return; }
    if (a >= 0x03000000 && a < 0x03100000) return;            /* ROM */
    unk("wr32", a, v);
}

void bus_write8(uint32_t a, uint8_t v) {
    if (a < RAM_SIZE) {
        uint32_t w = ram_rd(a & ~3u), sh = (3 - (a & 3)) * 8;
        ram_wr(a & ~3u, (w & ~(0xFFu << sh)) | ((uint32_t)v << sh));
        return;
    }
    if (a >= 0x03140000 && a < 0x03180000) { bus_write32(a & ~3u, v); return; }
    unk("wr8", a, v);
}

/* guest memory helpers for the chips (big-endian word view) */
uint32_t mem_rd32(uint32_t a) { a &= (RAM_SIZE - 1) & ~3u; return ram_rd(a); }
void mem_wr32(uint32_t a, uint32_t v) { a &= (RAM_SIZE - 1) & ~3u; ram_wr(a, v); }
uint16_t mem_rd16(uint32_t a) { uint32_t w = mem_rd32(a); return (a & 2) ? (uint16_t)w : (uint16_t)(w >> 16); }
void mem_wr16(uint32_t a, uint16_t v) {
    uint32_t w = mem_rd32(a);
    w = (a & 2) ? (w & 0xFFFF0000u) | v : (w & 0xFFFFu) | ((uint32_t)v << 16);
    mem_wr32(a, w);
}
uint8_t mem_rd8(uint32_t a) { return (uint8_t)(mem_rd32(a) >> ((3 - (a & 3)) * 8)); }

/* SPORT: VRAM serial-port page operations (flash fill / copy), 2KB pages */
void sport_write(uint32_t off, uint32_t v) {
    uint32_t page = (off & 0x1FFFF) ? off : off;   /* address bits select destination page */
    uint32_t dst = (page << 2) & 0xFFFFF;          /* SPORT window maps VRAM pages */
    dst = 0x200000 + (dst & ~0x7FFu);
    (void)dst;
    if (sys.sport_mode == 1) {                     /* flash write: fill page with colour latch */
        for (uint32_t i = 0; i < 0x800; i += 4) mem_wr32(dst + i, sys.sport_color);
    } else if (sys.sport_mode == 0 && sys.sport_src) {
        memmove(sys.ram + dst, sys.ram + sys.sport_src, 0x800);
    }
    unk("sport", off, v);
}

void sys_init(void) {
    memset(&sys.ram[0], 0, RAM_SIZE);
    sys.rom_overlay = 1;
    madam_reset();
    clio_reset();
    arm_reset();
}

int sys_load_bios(const uint8_t *data, size_t len) {
    if (len != ROM_SIZE && len != ROM_SIZE / 2) return -1;
    memset(sys.rom_w, 0, sizeof sys.rom_w);
    /* ROM image is big-endian; store as host words */
    for (size_t i = 0; i + 3 < len; i += 4)
        sys.rom_w[i >> 2] = ((uint32_t)data[i] << 24) | ((uint32_t)data[i + 1] << 16) | ((uint32_t)data[i + 2] << 8) | data[i + 3];
    return 0;
}

/* one video frame: 263 lines NTSC, CLIO drives vint0/vint1 at programmed lines */
#define CPU_HZ 12500000
#define LINES 263
void sys_run_frame(void) {
    int per_line = CPU_HZ / 60 / LINES;
    for (int line = 0; line < LINES; line++) {
        clio_set_line(line);
        arm_run(per_line);
        clio_tick(per_line);
        if (line < 240) video_scanline(line);
        arm.fiq_line = clio_fiq_pending();
        if (arm.halted) break;
    }
    sys.frame++;
}
