#pragma once
#include <stdint.h>
typedef struct XbusDev XbusDev;
struct XbusDev {
    uint32_t (*poll)(XbusDev *);
    uint8_t  (*status_byte)(XbusDev *);
    uint8_t  (*data_byte)(XbusDev *);
    void     (*cmd_byte)(XbusDev *, uint8_t);
    uint32_t poll_en;
};
extern XbusDev *xbus_dev[16];
extern int xbus_log;
uint32_t xbus_read(uint32_t off);
void xbus_write(uint32_t off, uint32_t v);
int xbus_irq_pending(void);
void cdrom_init(void);

struct Disc;
void cdrom_insert(struct Disc *d);
struct Disc *cdrom_disc(void);
