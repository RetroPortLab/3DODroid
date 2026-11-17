/* 3DODroid - XBUS expansion bus (CLIO 0x400-0x5FF) with the CD-ROM drive.
 *
 *   0x500  select device (low nibble = unit; bit 7 is used by the BIOS's
 *          second enumeration pass and doesn't change which unit answers)
 *   0x540  poll: status bits of the selected device; low bits are
 *          interrupt enables written by the host
 *            0x80 attention   0x40 data ready   0x10 status bytes waiting
 *          enable bit n (0x01..0x08) gates status bit n+4; a pending,
 *          enabled condition raises CLIO interrupt 0 bit 2.
 *          Unit 0x8F reads bus-level status; the BIOS treats 0x80/0x10 there
 *          as an error and otherwise scans each device's own poll register.
 *   0x580  command FIFO (write) / status FIFO (read), byte-wide
 *   0x5C0  data FIFO (read)
 *
 * Selecting an empty slot and reading its poll register raises a data abort
 * (bus timeout); the BIOS enumeration relies on that to find the end.
 * Behaviour worked out by watching the BIOS XBUS driver; the CD drive
 * personality lives in cdrom.c. */
#include "sys3do.h"
#include "xbus.h"
#include <string.h>
#include <stdio.h>

XbusDev *xbus_dev[16];
static uint32_t sel;
int xbus_log;
uint32_t xbus_dma_count, xbus_last_dma_dest;
void (*xbus_dma_hook)(uint32_t dest, uint32_t count);

static XbusDev *cur(void) { return sel == 0x8F ? NULL : xbus_dev[sel & 15]; }

/* enabled-and-pending: enable bit n gates status bit n+4 */
static int dev_irq(XbusDev *d) { return (d->poll(d) & (d->poll_en << 4) & 0xF0) != 0; }

static void logx(const char *what, uint32_t off, uint32_t v) {
    if (xbus_log) fprintf(stderr, "[xbus] %s %03x = %02x (sel %02x) pc=%08x\n", what, off, v, sel, arm.cur_pc);
}

uint32_t xbus_read(uint32_t off) {
    XbusDev *d = cur();
    uint32_t v = 0;
    switch (off) {
    case 0x500: v = sel; break;
    case 0x540:
        if (sel == 0x8F) v = 0;          /* bus status: 0x80/0x10 would flag a bus error */
        else if (d) v = d->poll(d) | d->poll_en;
        else arm.abort_req = 1;      /* empty slot: bus timeout -> data abort */
        break;
    case 0x580: v = d ? d->status_byte(d) : 0; break;
    case 0x5C0: v = d ? d->data_byte(d) : 0; break;
    }
    logx("rd", off, v);
    arm.fiq_line = clio_fiq_pending();
    return v;
}

void xbus_write(uint32_t off, uint32_t v) {
    XbusDev *d = cur();
    logx("wr", off, v);
    switch (off) {
    case 0x500: sel = v & 0xFF; break;
    case 0x540:
        if (d) d->poll_en = v & 0x0F;
        if (d && (v & 0x80) && d == xbus_dev[0]) cdrom_ack_attention();
        break;
    case 0x580: if (d) d->cmd_byte(d, (uint8_t)v); break;
    }
    arm.fiq_line = clio_fiq_pending();
}

int xbus_irq_pending(void) {
    for (int i = 0; i < 16; i++) {
        XbusDev *d = xbus_dev[i];
        if (d && dev_irq(d)) return 1;
    }
    return 0;
}

/* DMA from the selected device's data FIFO into RAM (CLIO 0x304 bit 20). */
void xbus_dma_from_device(uint32_t dest, uint32_t count) {
    XbusDev *d = cur();
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b = d ? d->data_byte(d) : 0;
        uint32_t a = dest + i;
        if (a < RAM_SIZE) {
            uint32_t w = mem_rd32(a & ~3u), sh = (3 - (a & 3)) * 8;
            mem_wr32(a & ~3u, (w & ~(0xFFu << sh)) | ((uint32_t)b << sh));
        }
    }
    xbus_dma_count++; xbus_last_dma_dest = dest;
    if (xbus_dma_hook) xbus_dma_hook(dest, count);
    if (xbus_log) fprintf(stderr, "[xbus] dma %u bytes -> %08x\n", count, dest);
}
