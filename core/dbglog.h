/* Debug event log: a ring of text lines the Android app can save for bug
 * reports. Cheap enough to leave on: only rare events are recorded, and
 * dbg_once() records each distinct event a single time. */
#ifndef DBGLOG_H
#define DBGLOG_H
#include <stddef.h>
#include <stdint.h>

void   dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void   dbg_stream(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* frequent events */
int    dbg_once(uint32_t kind, uint32_t key);    /* 1 the first time this kind/key is seen */
size_t dbg_dump(char *out, size_t max);          /* oldest first, NUL-terminated */
void   dbg_clear(void);
extern int dbg_echo;                             /* also print to stderr (desktop) */
#endif
