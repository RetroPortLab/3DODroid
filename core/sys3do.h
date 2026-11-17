/* 3DODroid - system state shared by all chips */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arm60.h"

#define RAM_SIZE   0x00300000u   /* 2MB DRAM + 1MB VRAM, contiguous */
#define ROM_SIZE   0x00100000u
#define NVRAM_SIZE 0x8000u

typedef struct {
    uint8_t  ram[RAM_SIZE];      /* stored as host-endian 32-bit words */
    uint32_t rom_w[ROM_SIZE / 4];
    uint8_t  nvram[NVRAM_SIZE];
    int nvram_dirty;
    int rom_overlay;
    int log_unknown, unk_logged, log_irq, log_cap;
    uint32_t irq_watch;
    uint32_t xbus_dma_irq;
    uint32_t watch_lo, watch_hi;      /* debug: log CPU reads in this range */            /* bring-up: which CLIO bit signals XBUS DMA done */               /* debug: backtrace when these mask0 bits are cleared */
    uint32_t sport_color;
    uint8_t sport_buf[0x800];
    uint64_t frame;
    uint32_t fb[320 * 240];      /* last presented frame, XRGB8888 */
} Sys3do;

extern Sys3do sys;

void sys_init(void);
int  sys_load_bios(const uint8_t *data, size_t len);
void sys_run_frame(void);
void sys_backtrace(const char *why);

uint32_t mem_rd32(uint32_t a);
void     mem_wr32(uint32_t a, uint32_t v);
uint16_t mem_rd16(uint32_t a);
void     mem_wr16(uint32_t a, uint16_t v);
uint8_t  mem_rd8(uint32_t a);

/* MADAM: memory controller, cel engine, matrix engine, DMA */
void madam_reset(void);
uint32_t madam_read(uint32_t off);
void madam_write(uint32_t off, uint32_t v);
uint32_t madam_reg(uint32_t off);
void madam_set_reg(uint32_t off, uint32_t v);

/* CLIO: interrupts, timers, video counters, DSP, XBUS */
void clio_reset(void);
uint32_t clio_read(uint32_t off);
void clio_write(uint32_t off, uint32_t v);
void clio_set_line(int line);
void clio_tick(int cycles);
int  clio_fiq_pending(void);
void clio_raise(int word, uint32_t bits);

uint32_t sport_read(uint32_t off);
void sport_write(uint32_t off, uint32_t mask);
void video_scanline(int line);
