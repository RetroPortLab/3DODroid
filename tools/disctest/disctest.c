/* Disc backend self-test: opens images and checks synthetic sector patterns
 * produced by make_test_disc.py (data: BE lba + (lba*7+i); audio: lba*3+i). */
#include "../../core/disc.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    int fails = 0;
    for (int a = 1; a < argc; a++) {
        char err[256]; Disc *d = disc_open(argv[a], err, sizeof err);
        if (!d) { printf("%s: %s\n", argv[a], err); fails++; continue; }
        printf("%s: %d track(s), leadout %u\n", argv[a], disc_track_count(d), disc_total_sectors(d));
        for (int i = 0; i < disc_track_count(d); i++) {
            const DiscTrack *t = disc_track(d, i);
            printf("  track %d %s start %u len %u\n", t->number, t->type == TRK_AUDIO ? "AUDIO" : "DATA", t->start, t->length);
            int bad = 0;
            for (uint32_t s = 0; s < t->length; s++) {
                uint32_t lba = t->start + s;
                if (t->type != TRK_AUDIO) {
                    uint8_t b[2048]; if (disc_read_data(d, lba, b)) { bad++; continue; }
                    uint32_t v = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
                    if (v != lba) bad++;
                    for (int k = 4; k < 2048; k++) if (b[k] != (uint8_t)(lba * 7 + k)) { bad++; break; }
                } else {
                    uint8_t b[2352]; if (disc_read_raw(d, lba, b)) { bad++; continue; }
                    uint32_t rel = s + 150 * 0;  /* pattern index = sector within audio file incl. 2s INDEX 00 */
                    rel = s + 150;
                    for (int k = 0; k < 2352; k++) if (b[k] != (uint8_t)(rel * 3 + k)) { bad++; break; }
                }
            }
            printf("    %s (%d bad sectors)\n", bad ? "FAIL" : "ok", bad);
            fails += bad != 0;
        }
        disc_close(d);
    }
    return fails != 0;
}
