/* 3DODroid - disc image backends.
 *
 *   .iso        single MODE1/2048 track
 *   .bin        single track; 2048 or 2352 bytes/sector guessed from size/sync
 *   .cue        BIN/CUE sheets, multiple FILE/TRACK entries, MODE1/2048,
 *               MODE1/2352, MODE2/2352, AUDIO, PREGAP and INDEX 00/01
 *   .chd        MAME CHD v5 CD images via libchdr (CHT2/CHTR metadata)
 */
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <libchdr/chd.h>

typedef struct {
    FILE *f;            /* backing file (BIN/ISO) */
    uint64_t file_off;  /* byte offset of this track's INDEX 01 sector */
    int sector_size;    /* 2048 or 2352 */
    int data_off;       /* offset of user data within a stored sector */
} TrackSrc;

struct Disc {
    int ntracks;
    DiscTrack t[DISC_MAX_TRACKS];
    TrackSrc src[DISC_MAX_TRACKS];
    uint32_t leadout;
    FILE *files[DISC_MAX_TRACKS]; int nfiles;
    /* CHD */
    chd_file *chd;
    uint32_t chd_frame_base[DISC_MAX_TRACKS];   /* CHD frame index of track LBA start */
    int chd_swap_audio[DISC_MAX_TRACKS];
    uint8_t *hunk; uint32_t hunk_bytes, frames_per_hunk, cur_hunk;
};

static void seterr(char *err, int n, const char *msg, const char *arg) {
    if (err) snprintf(err, (size_t)n, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

static const char *ext_of(const char *p) { const char *d = strrchr(p, '.'); return d ? d + 1 : ""; }

static uint64_t file_size(FILE *f) { long cur = ftell(f); fseek(f, 0, SEEK_END); uint64_t s = (uint64_t)ftell(f); fseek(f, cur, SEEK_SET); return s; }

static const uint8_t sync_pat[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

/* ---------- ISO / single BIN ---------- */
static Disc *open_flat(const char *path, char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { seterr(err, errlen, "can't open", path); return NULL; }
    Disc *d = calloc(1, sizeof *d);
    d->files[d->nfiles++] = f;
    uint8_t head[16] = {0};
    if (fread(head, 1, 16, f) != 16) { /* tiny file: still try */ }
    uint64_t sz = file_size(f);
    int raw = !memcmp(head, sync_pat, 12) || (sz % 2352 == 0 && sz % 2048 != 0);
    d->ntracks = 1;
    d->t[0] = (DiscTrack){ 1, TRK_MODE1, 0, 0 };
    d->src[0] = (TrackSrc){ f, 0, raw ? 2352 : 2048, raw ? 16 : 0 };
    if (raw && head[15] == 2) { d->t[0].type = TRK_MODE2; d->src[0].data_off = 24; }
    d->t[0].length = (uint32_t)(sz / (uint64_t)d->src[0].sector_size);
    d->leadout = d->t[0].length;
    return d;
}

/* ---------- CUE ---------- */
static int parse_msf(const char *s) { int m = 0, sec = 0, f = 0; sscanf(s, "%d:%d:%d", &m, &sec, &f); return (m * 60 + sec) * 75 + f; }

static Disc *open_cue(const char *path, char *err, int errlen) {
    FILE *cf = fopen(path, "r");
    if (!cf) { seterr(err, errlen, "can't open", path); return NULL; }
    char dir[1024]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (!sl) sl = strrchr(dir, '\\');
    if (sl) sl[1] = 0; else dir[0] = 0;

    Disc *d = calloc(1, sizeof *d);
    FILE *curf = NULL; uint64_t curf_size = 0;
    uint32_t file_lba_base = 0;        /* disc LBA where the current FILE starts */
    uint32_t pregap_total = 0;         /* PREGAP sectors not present in files */
    int ti = -1; int idx1_seen = 0;
    char line[1024];

    while (fgets(line, sizeof line, cf)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!strncasecmp(p, "FILE", 4)) {
            char name[900] = {0};
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) { memcpy(name, q1 + 1, (size_t)(q2 - q1 - 1)); }
            else sscanf(p + 4, " %899s", name);
            char full[2048]; snprintf(full, sizeof full, "%s%s", (name[0] == '/' ? "" : dir), name);
            if (curf) {                                     /* close out previous file's span */
                file_lba_base += (uint32_t)(curf_size / (uint64_t)(d->src[ti >= 0 ? ti : 0].sector_size ? d->src[ti >= 0 ? ti : 0].sector_size : 2352));
            }
            curf = fopen(full, "rb");
            if (!curf) { seterr(err, errlen, "missing track file", full); fclose(cf); disc_close(d); return NULL; }
            d->files[d->nfiles++] = curf;
            curf_size = file_size(curf);
        } else if (!strncasecmp(p, "TRACK", 5)) {
            int num = 0; char type[32] = {0};
            sscanf(p + 5, "%d %31s", &num, type);
            if (++ti >= DISC_MAX_TRACKS) break;
            d->t[ti].number = num;
            TrackSrc *s = &d->src[ti]; s->f = curf;
            if (!strncasecmp(type, "AUDIO", 5)) { d->t[ti].type = TRK_AUDIO; s->sector_size = 2352; s->data_off = 0; }
            else if (!strncasecmp(type, "MODE1/2048", 10)) { d->t[ti].type = TRK_MODE1; s->sector_size = 2048; s->data_off = 0; }
            else if (!strncasecmp(type, "MODE1/2352", 10)) { d->t[ti].type = TRK_MODE1; s->sector_size = 2352; s->data_off = 16; }
            else if (!strncasecmp(type, "MODE2/2352", 10)) { d->t[ti].type = TRK_MODE2; s->sector_size = 2352; s->data_off = 24; }
            else { seterr(err, errlen, "unsupported track type", type); fclose(cf); disc_close(d); return NULL; }
            idx1_seen = 0;
        } else if (!strncasecmp(p, "PREGAP", 6) && ti >= 0) {
            pregap_total += (uint32_t)parse_msf(p + 6);
        } else if (!strncasecmp(p, "INDEX", 5) && ti >= 0) {
            int n = 0; char msf[32] = {0};
            sscanf(p + 5, "%d %31s", &n, msf);
            if (n == 1 && !idx1_seen) {
                idx1_seen = 1;
                uint32_t in_file = (uint32_t)parse_msf(msf);
                d->src[ti].file_off = (uint64_t)in_file * (uint64_t)d->src[ti].sector_size;
                d->t[ti].start = file_lba_base + in_file + pregap_total;
            }
        }
    }
    fclose(cf);
    d->ntracks = ti + 1;
    if (d->ntracks <= 0) { seterr(err, errlen, "no tracks in cue", path); disc_close(d); return NULL; }
    /* lengths: up to the next track's INDEX 01 in the same file, else end of file */
    for (int i = 0; i < d->ntracks; i++) {
        TrackSrc *s = &d->src[i];
        uint64_t end = file_size(s->f);
        uint32_t len = (uint32_t)((end - s->file_off) / (uint64_t)s->sector_size);
        if (i + 1 < d->ntracks && d->src[i + 1].f == s->f) {
            uint32_t next_in_file = (uint32_t)(d->src[i + 1].file_off / (uint64_t)d->src[i + 1].sector_size);
            uint32_t this_in_file = (uint32_t)(s->file_off / (uint64_t)s->sector_size);
            len = next_in_file - this_in_file;
        }
        d->t[i].length = len;
    }
    DiscTrack *last = &d->t[d->ntracks - 1];
    d->leadout = last->start + last->length;
    return d;
}

/* ---------- CHD ---------- */
#define CHD_FRAME 2448            /* 2352 + 96 subcode */
static Disc *open_chd(const char *path, char *err, int errlen) {
    chd_file *chd = NULL;
    chd_error e = chd_open(path, CHD_OPEN_READ, NULL, &chd);
    if (e != CHDERR_NONE) { seterr(err, errlen, "can't open CHD", chd_error_string(e)); return NULL; }
    const chd_header *h = chd_get_header(chd);
    Disc *d = calloc(1, sizeof *d);
    d->chd = chd;
    d->hunk_bytes = h->hunkbytes;
    d->frames_per_hunk = h->hunkbytes / CHD_FRAME;
    d->hunk = malloc(h->hunkbytes);
    d->cur_hunk = 0xFFFFFFFFu;
    if (!d->frames_per_hunk) { seterr(err, errlen, "not a CD CHD", path); disc_close(d); return NULL; }

    uint32_t frame = 0, lba = 0;
    for (int i = 0; i < DISC_MAX_TRACKS; i++) {
        char meta[256] = {0}; uint32_t got = 0;
        int tnum = 0, frames = 0, pregap = 0, postgap = 0;
        char type[32] = {0}, sub[32] = {0}, pgtype[32] = {0}, pgsub[32] = {0};
        if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, (uint32_t)i, meta, sizeof meta - 1, &got, NULL, NULL) == CHDERR_NONE) {
            sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &tnum, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
        } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, (uint32_t)i, meta, sizeof meta - 1, &got, NULL, NULL) == CHDERR_NONE) {
            sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &tnum, type, sub, &frames);
        } else break;

        DiscTrack *t = &d->t[i];
        t->number = tnum;
        if (!strcmp(type, "AUDIO")) t->type = TRK_AUDIO;
        else if (!strncmp(type, "MODE2", 5)) t->type = TRK_MODE2;
        else t->type = TRK_MODE1;
        TrackSrc *s = &d->src[i];
        if (!strcmp(type, "MODE1")) { s->sector_size = 2048; s->data_off = 0; }
        else if (!strcmp(type, "MODE1_RAW")) { s->sector_size = 2352; s->data_off = 16; }
        else if (!strcmp(type, "MODE2_RAW") || !strcmp(type, "MODE2_FORM_MIX")) { s->sector_size = 2352; s->data_off = 24; }
        else if (!strncmp(type, "MODE2", 5)) { s->sector_size = 2336; s->data_off = 8; }
        else { s->sector_size = 2352; s->data_off = 0; }
        d->chd_swap_audio[i] = t->type == TRK_AUDIO;

        /* pregap stored in the CHD data only for 'V' pregap types */
        int stored_pregap = (pgtype[0] == 'V') ? pregap : 0;
        if (i > 0 && !stored_pregap) lba += (uint32_t)pregap;
        t->start = lba + (uint32_t)stored_pregap;
        t->length = (uint32_t)(frames - stored_pregap);
        d->chd_frame_base[i] = frame + (uint32_t)stored_pregap;
        lba += (uint32_t)frames;
        frame += (uint32_t)((frames + 3) & ~3);   /* tracks padded to 4 frames */
        d->ntracks = i + 1;
    }
    if (!d->ntracks) { seterr(err, errlen, "CHD has no CD track metadata", path); disc_close(d); return NULL; }
    d->leadout = d->t[d->ntracks - 1].start + d->t[d->ntracks - 1].length;
    return d;
}

static int chd_frame(Disc *d, int ti, uint32_t lba, const uint8_t **out) {
    uint32_t fr = d->chd_frame_base[ti] + (lba - d->t[ti].start);
    uint32_t hunk = fr / d->frames_per_hunk;
    if (hunk != d->cur_hunk) {
        if (chd_read(d->chd, hunk, d->hunk) != CHDERR_NONE) return -1;
        d->cur_hunk = hunk;
    }
    *out = d->hunk + (fr % d->frames_per_hunk) * CHD_FRAME;
    return 0;
}

/* ---------- common ---------- */
Disc *disc_open(const char *path, char *err, int errlen) {
    const char *e = ext_of(path);
    if (!strcasecmp(e, "chd")) return open_chd(path, err, errlen);
    if (!strcasecmp(e, "cue")) return open_cue(path, err, errlen);
    if (!strcasecmp(e, "iso") || !strcasecmp(e, "bin") || !strcasecmp(e, "img")) return open_flat(path, err, errlen);
    seterr(err, errlen, "unknown disc image type", e);
    return NULL;
}

void disc_close(Disc *d) {
    if (!d) return;
    for (int i = 0; i < d->nfiles; i++) if (d->files[i]) fclose(d->files[i]);
    if (d->chd) chd_close(d->chd);
    free(d->hunk);
    free(d);
}

int disc_track_count(const Disc *d) { return d->ntracks; }
const DiscTrack *disc_track(const Disc *d, int i) { return (i >= 0 && i < d->ntracks) ? &d->t[i] : NULL; }
uint32_t disc_total_sectors(const Disc *d) { return d->leadout; }

static int find_track(const Disc *d, uint32_t lba) {
    for (int i = 0; i < d->ntracks; i++)
        if (lba >= d->t[i].start && lba < d->t[i].start + d->t[i].length) return i;
    return -1;
}

int disc_read_data(Disc *d, uint32_t lba, uint8_t *out) {
    int ti = find_track(d, lba);
    if (ti < 0 || d->t[ti].type == TRK_AUDIO) { memset(out, 0, 2048); return -1; }
    TrackSrc *s = &d->src[ti];
    if (d->chd) {
        const uint8_t *fr;
        if (chd_frame(d, ti, lba, &fr)) return -1;
        memcpy(out, fr + s->data_off, 2048);
        return 0;
    }
    uint64_t off = s->file_off + (uint64_t)(lba - d->t[ti].start) * (uint64_t)s->sector_size + (uint64_t)s->data_off;
    if (fseek(s->f, (long)off, SEEK_SET) || fread(out, 1, 2048, s->f) != 2048) return -1;
    return 0;
}

int disc_read_raw(Disc *d, uint32_t lba, uint8_t *out) {
    int ti = find_track(d, lba);
    if (ti < 0) { memset(out, 0, 2352); return -1; }
    TrackSrc *s = &d->src[ti];
    if (d->chd) {
        const uint8_t *fr;
        if (chd_frame(d, ti, lba, &fr)) return -1;
        if (s->sector_size == 2352) {
            memcpy(out, fr, 2352);
            if (d->chd_swap_audio[ti]) for (int i = 0; i < 2352; i += 2) { uint8_t t = out[i]; out[i] = out[i + 1]; out[i + 1] = t; }
        } else { memset(out, 0, 2352); memcpy(out + 16, fr, 2048); }
        return 0;
    }
    if (s->sector_size == 2352) {
        uint64_t off = s->file_off + (uint64_t)(lba - d->t[ti].start) * 2352u;
        if (fseek(s->f, (long)off, SEEK_SET) || fread(out, 1, 2352, s->f) != 2352) return -1;
        return 0;
    }
    memset(out, 0, 2352);
    return disc_read_data(d, lba, out + 16);
}
