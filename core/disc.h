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
/* Same, with the image type given separately ("chd", "cue", "iso", "bin"), for
 * paths without a usable extension such as /proc/self/fd/N on Android. */
Disc *disc_open_as(const char *path, const char *type, char *err, int errlen);
/* Optional: maps a CUE sheet's FILE name to an openable path (Android passes
 * /proc/self/fd/N for files it opened through the storage framework).
 * Return NULL to use the name relative to the cue's folder. */
extern const char *(*disc_cue_resolver)(const char *name);
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
