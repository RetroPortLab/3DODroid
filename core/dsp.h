/* 3DO audio DSP (the "DSPP" in CLIO): a 16-bit-instruction processor that
 * runs its program from the top once per 44.1 kHz sample frame, fed by 13
 * RAM->DSP DMA channels and writing stereo samples to the DAC. */
#ifndef DSP_H
#define DSP_H
#include <stdint.h>

void     dsp_reset(void);
void     dsp_sample(void);                      /* run one sample frame */

/* ARM side, at CLIO offsets 0x17xx and 0x1800-0x3FFF */
int      dsp_clio_write(uint32_t off, uint32_t v);   /* 1 if handled */
int      dsp_clio_read(uint32_t off, uint32_t *v);   /* 1 if handled */

/* DMA channels, at MADAM 0x400-0x4CF (into the DSP) and 0x500-0x53F (out) */
void     dsp_fifo_write(uint32_t off, uint32_t v);
uint32_t dsp_fifo_read(uint32_t off);
void     dsp_fifo_stop(uint32_t mask);          /* CLIO 0x300: stop and clear channels */

/* Audio output: stereo int16 frames, 44100 Hz. Returns frames copied. */
int      dsp_audio_take(int16_t *out, int max_frames);
int      dsp_audio_available(void);

extern int dsp_log;
#endif
