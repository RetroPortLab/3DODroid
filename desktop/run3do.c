/* 3DODroid desktop runner (headless), the equivalent of JagDroid's jagrun.
 *   run3do <bios.bin> [frames] [--trace] [--shot out.ppm]
 */
#include "../core/sys3do.h"
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: run3do bios.bin [frames] [--trace] [--shot file.ppm]\n"); return 1; }
    size_t len; uint8_t *bios = slurp(argv[1], &len);
    if (!bios) { perror(argv[1]); return 1; }
    int frames = 120; const char *shot = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) sys.log_unknown = 1;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else frames = atoi(argv[i]);
    }
    sys_init();
    if (sys_load_bios(bios, len)) { fprintf(stderr, "BIOS must be 1MB (got %zu bytes)\n", len); return 1; }
    for (int f = 0; f < frames && !arm.halted; f++) sys_run_frame();
    fprintf(stderr, "frames=%llu cycles=%llu pc=%08x mode=%02x\n",
            (unsigned long long)sys.frame, (unsigned long long)arm.cycles, arm.r[15], arm.mode);
    if (shot) write_ppm(shot);
    return 0;
}
