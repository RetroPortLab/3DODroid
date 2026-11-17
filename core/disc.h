/* 3DODroid - disc images: ISO, BIN/CUE, CHD */
#pragma once
#include <stdint.h>

#define DISC_MAX_TRACKS 99

typedef enum { TRK_AUDIO = 0, TRK_MODE1 = 1, TRK_MODE2 = 2 } TrackType;

typedef struct {
    int number;            /* 1-based */
    TrackType type;
    uint32_t start;        /* LBA of INDEX 01 (0 = MSF 00:02:00) */
    uint32_t length;       /* sectors */
} DiscTrack;

typedef struct Disc Disc;

/* Opens .iso, .bin, .cue or .chd. Returns NULL and fills err on failure. */
Disc *disc_open(const char *path, char *err, int errlen);
void disc_close(Disc *d);

int disc_track_count(const Disc *d);
const DiscTrack *disc_track(const Disc *d, int i);   /* i: 0-based */
uint32_t disc_total_sectors(const Disc *d);        /* lead-out LBA */

/* 2048-byte user data of a data sector; returns 0 on success */
int disc_read_data(Disc *d, uint32_t lba, uint8_t *out2048);
/* 2352-byte raw sector (audio as little-endian 16-bit stereo); 0 on success */
int disc_read_raw(Disc *d, uint32_t lba, uint8_t *out2352);

static inline void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
    lba += 150; *m = (uint8_t)(lba / 4500); *s = (uint8_t)((lba / 75) % 60); *f = (uint8_t)(lba % 75);
}
