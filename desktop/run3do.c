/* 3DODroid desktop runner (headless), the equivalent of JagDroid's jagrun.
 *   run3do <bios.bin> [frames] [--disc image] [--trace] [--shot out.ppm]
 */
#include "../core/sys3do.h"
#include "../core/xbus.h"
#include "../core/disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len); if (fread(b, 1, *len, f) != *len) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

static void write_ppm(const char *p) {
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n320 240\n255\n");
    for (int i = 0; i < 320 * 240; i++) { uint32_t c = sys.fb[i]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); }
    fclose(f);
}

/* scripted input: --press NAME@frame[+len] (default hold 10 frames) */
typedef struct { uint32_t bits; int at, len; } Press;
static Press presses[64]; static int npress;
static uint32_t button_bit(const char *n) {
    static const struct { const char *n; uint32_t b; } t[] = {
        {"DOWN",0x80000000u},{"UP",0x40000000u},{"RIGHT",0x20000000u},{"LEFT",0x10000000u},
        {"A",0x08000000u},{"B",0x04000000u},{"C",0x02000000u},{"START",0x01000000u},{"P",0x01000000u},
        {"X",0x00800000u},{"R",0x00400000u},{"L",0x00200000u} };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].b;
    return 0;
}

static long trace_left;
static void on_branch(uint32_t from, uint32_t to) {
    if (from >= 0x100 && from < 0x130 && to >= 0x100 && to < 0x130) return;
    if (trace_left > 0) { trace_left--; fprintf(stderr, "  br %08x -> %08x  mode=%02x\n", from, to, arm.mode); }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: run3do bios.bin [frames] [--disc image.chd|.cue|.iso] [--trace] [--shot file.ppm]\n"); return 1; }
    size_t len; uint8_t *bios = slurp(argv[1], &len);
    if (!bios) { perror(argv[1]); return 1; }
    int frames = 120, shot_every = 0; const char *shot = NULL, *disc_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) { sys.log_unknown = 1; if (!sys.log_cap) sys.log_cap = 400; }
        else if (!strcmp(argv[i], "--tracecap") && i + 1 < argc) sys.log_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xbus")) xbus_log = 1;
        else if (!strcmp(argv[i], "--irq")) sys.log_irq = 1;
        else if (!strcmp(argv[i], "--dsp")) sys.log_dsp = 1;
        else if (!strcmp(argv[i], "--dsptick")) sys.dsp_tick_hle = 1;
        else if (!strcmp(argv[i], "--dmairq") && i + 1 < argc) sys.xbus_dma_irq = 1u << atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc_path = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 64) {
            char name[16] = {0}; int at = 0, len = 10;
            if (sscanf(argv[++i], "%15[A-Z]@%d+%d", name, &at, &len) >= 2)
                presses[npress++] = (Press){ button_bit(name), at, len };
        }
        else if (!strcmp(argv[i], "--shotevery") && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--branches") && i + 1 < argc) { trace_left = atol(argv[++i]); arm.trace_hook = on_branch; }
        else frames = atoi(argv[i]);
    }
    void (*hook)(uint32_t, uint32_t) = arm.trace_hook;
    sys_init();
    arm.trace_hook = hook;
    arm.log_undef = 5;
    if (sys_load_bios(bios, len)) { fprintf(stderr, "BIOS must be 1MB (got %zu bytes)\n", len); return 1; }
    if (disc_path) {
        char err[256]; Disc *d = disc_open(disc_path, err, sizeof err);
        if (!d) { fprintf(stderr, "disc: %s\n", err); return 1; }
        fprintf(stderr, "disc: %d track(s), %u sectors\n", disc_track_count(d), disc_total_sectors(d));
        cdrom_insert(d);
    }
    for (int f = 0; f < frames && !arm.halted; f++) {
        pad_buttons[0] = 0;
        for (int k = 0; k < npress; k++)
            if (f >= presses[k].at && f < presses[k].at + presses[k].len) pad_buttons[0] |= presses[k].bits;
        sys_run_frame();
        if (shot && shot_every && f && f % shot_every == 0) {
            char p[512]; snprintf(p, sizeof p, "%s.%05d.ppm", shot, f); write_ppm(p);
        }
    }
    fprintf(stderr, "frames=%llu cycles=%llu pc=%08x mode=%02x\n",
            (unsigned long long)sys.frame, (unsigned long long)arm.cycles, arm.r[15], arm.mode);
    if (shot) write_ppm(shot);
    return 0;
}
