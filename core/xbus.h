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
uint8_t cdrom_data_byte(void);
void xbus_dma_from_device(uint32_t dest, uint32_t count);
extern uint32_t xbus_dma_count, xbus_last_dma_dest;
extern void (*xbus_dma_hook)(uint32_t dest, uint32_t count);
void cdrom_ack_attention(void);
