#include "dbglog.h"
#include "sys3do.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int dbg_echo;

typedef struct { char *buf; size_t size, pos, used; } Ring;
static char rare_buf[256 * 1024], stream_buf[256 * 1024];
static Ring rare = { rare_buf, sizeof rare_buf, 0, 0 }, stream = { stream_buf, sizeof stream_buf, 0, 0 };

static void put(Ring *r, int to_stream, const char *fmt, va_list ap) {
    char line[256];
    int n = snprintf(line, sizeof line, "f%-7llu ", (unsigned long long)sys.frame);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    if (m < 0) return;
    n += m;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    line[n] = 0;
    if (dbg_echo) fputs(line, stderr);
    (void)to_stream;
    for (int i = 0; i < n; i++) { r->buf[r->pos] = line[i]; r->pos = (r->pos + 1) % r->size; }
    r->used = r->used + (size_t)n > r->size ? r->size : r->used + (size_t)n;
}

void dbg(const char *fmt, ...) { va_list ap; va_start(ap, fmt); put(&rare, 0, fmt, ap); va_end(ap); }
void dbg_stream(const char *fmt, ...) { va_list ap; va_start(ap, fmt); put(&stream, 1, fmt, ap); va_end(ap); }

#define SEEN 8192
static uint64_t seen[SEEN];

int dbg_once(uint32_t kind, uint32_t key) {
    uint64_t k = ((uint64_t)kind << 32 | key) + 1;
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 51) & (SEEN - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (SEEN - 1)) {
        if (seen[h] == k) return 0;
        if (!seen[h]) { seen[h] = k; return 1; }
    }
    return 0;                                    /* table crowded: stay quiet */
}

static size_t dump_ring(const Ring *r, char *out, size_t max) {
    size_t n = r->used < max ? r->used : max;
    size_t start = (r->pos + r->size - n) % r->size;
    for (size_t i = 0; i < n; i++) out[i] = r->buf[(start + i) % r->size];
    size_t skip = 0;                             /* drop a partial first line after wrap-around */
    if (r->used == r->size || n < r->used) { while (skip < n && out[skip] != '\n') skip++; if (skip < n) skip++; }
    if (skip) { memmove(out, out + skip, n - skip); n -= skip; }
    return n;
}

size_t dbg_dump(char *out, size_t max) {
    if (max < 64) return 0;
    size_t n = 0;
    const char *h1 = "==== Events (each kind recorded once, plus interrupts and CD commands) ====\n";
    const char *h2 = "\n==== Most recent disc reads and transfers ====\n";
    size_t l1 = strlen(h1), l2 = strlen(h2);
    memcpy(out, h1, l1); n += l1;
    n += dump_ring(&rare, out + n, (max - n - l2 - 1) / 2 > 0 ? (max - n - l2 - 1) / 2 : 0);
    memcpy(out + n, h2, l2); n += l2;
    n += dump_ring(&stream, out + n, max - n - 1);
    out[n] = 0;
    return n;
}

void dbg_clear(void) {
    rare.pos = rare.used = stream.pos = stream.used = 0;
    memset(seen, 0, sizeof seen);
}
