#pragma once
#include <stdint.h>
typedef struct {
    uint32_t first_ccb;
    int32_t hdx, hdy, vdx, vdy, hddx, hddy;
    uint32_t pixc, plut;
    uint32_t rbase, wbase, fb_stride, width_words_x2;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    uint64_t cels_drawn;
} CelState;
extern CelState cel;
void cel_run(void);
void cel_set_regs(uint32_t regctl0, uint32_t regctl1, uint32_t rbase, uint32_t wbase);
