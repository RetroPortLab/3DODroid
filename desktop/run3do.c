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

static long trace_left;
static void on_branch(uint32_t from, uint32_t to) {
    if (from >= 0x100 && from < 0x130 && to >= 0x100 && to < 0x130) return;
    if (trace_left > 0) { trace_left--; fprintf(stderr, "  br %08x -> %08x  mode=%02x\n", from, to, arm.mode); }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: run3do bios.bin [frames] [--disc image.chd|.cue|.iso] [--trace] [--shot file.ppm]\n"); return 1; }
    size_t len; uint8_t *bios = slurp(argv[1], &len);
    if (!bios) { perror(argv[1]); return 1; }
    int frames = 120; const char *shot = NULL, *disc_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) { sys.log_unknown = 1; if (!sys.log_cap) sys.log_cap = 400; }
        else if (!strcmp(argv[i], "--tracecap") && i + 1 < argc) sys.log_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--xbus")) xbus_log = 1;
        else if (!strcmp(argv[i], "--irq")) sys.log_irq = 1;
        else if (!strcmp(argv[i], "--dmairq") && i + 1 < argc) sys.xbus_dma_irq = 1u << atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc_path = argv[++i];
        else if (!strcmp(argv[i], "--branches") && i + 1 < argc) { trace_left = atol(argv[++i]); arm.trace_hook = on_branch; }
        else frames = atoi(argv[i]);
    }
    void (*hook)(uint32_t, uint32_t) = arm.trace_hook;
    sys_init();
    arm.trace_hook = hook;
    if (sys_load_bios(bios, len)) { fprintf(stderr, "BIOS must be 1MB (got %zu bytes)\n", len); return 1; }
    if (disc_path) {
        char err[256]; Disc *d = disc_open(disc_path, err, sizeof err);
        if (!d) { fprintf(stderr, "disc: %s\n", err); return 1; }
        fprintf(stderr, "disc: %d track(s), %u sectors\n", disc_track_count(d), disc_total_sectors(d));
        cdrom_insert(d);
    }
    for (int f = 0; f < frames && !arm.halted; f++) sys_run_frame();
    fprintf(stderr, "frames=%llu cycles=%llu pc=%08x mode=%02x\n",
            (unsigned long long)sys.frame, (unsigned long long)arm.cycles, arm.r[15], arm.mode);
    if (shot) write_ppm(shot);
    return 0;
}
