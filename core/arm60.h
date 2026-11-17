/* 3DODroid - ARM60 (ARMv3) CPU core */
#pragma once
#include <stdint.h>

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };

typedef struct {
    uint32_t r[16];
    uint32_t n, z, c, v, i, f, mode;   /* CPSR split out */
    uint32_t spsr;                     /* SPSR of current mode */
    /* banked storage */
    uint32_t bank_usr[7];              /* r8-r14 for usr/sys */
    uint32_t bank_fiq[7];              /* r8-r14 for fiq */
    uint32_t bank_r13[6], bank_r14[6], bank_spsr[6];   /* idx: usr,svc,abt,und,irq,fiq */
    int irq_line, fiq_line;
    int halted;
    int v4;                            /* test-only: ARMv4 extensions */
    uint32_t cur_pc;
    uint64_t cycles;
} Arm60;

extern Arm60 arm;

/* bus interface, implemented by bus.c */
uint32_t bus_read32(uint32_t a);
uint8_t  bus_read8(uint32_t a);
void     bus_write32(uint32_t a, uint32_t v);
void     bus_write8(uint32_t a, uint8_t v);
uint32_t bus_fetch(uint32_t a);

void arm_reset(void);
int  arm_run(int cycles);          /* returns cycles executed */
uint32_t arm_cpsr(void);
void arm_set_cpsr(uint32_t v);
