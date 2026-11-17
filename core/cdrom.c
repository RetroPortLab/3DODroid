/* 3DODroid - CD-ROM drive on the XBUS (bring-up: logs commands) */
#include "sys3do.h"
#include "xbus.h"
#include <stdio.h>
#include <string.h>

/* Replies are queued and only become visible after a delay, like a real
 * drive: the BIOS sends the next command before the previous status is due. */
#define CD_REPLY_DELAY 25000          /* CPU cycles, ~2ms */
#define CD_QLEN 8
typedef struct { uint8_t b[64]; int len; uint64_t ready_at; } Reply;
typedef struct {
    XbusDev dev;
    uint8_t cmd[7]; int cmd_len;
    Reply q[CD_QLEN]; int q_head, q_count, st_pos;
    Reply *building;
} Cd;
static Cd cd;

static Reply *head_ready(void) {
    if (!cd.q_count) return NULL;
    Reply *r = &cd.q[cd.q_head];
    return arm.cycles >= r->ready_at ? r : NULL;
}

/* drive status: door closed, disc present, spun up, ready */
static uint8_t cd_status_byte(void) { return 0x00; }

static void push(uint8_t b) { Reply *r = cd.building; if (r && r->len < 64) r->b[r->len++] = b; }

static void exec_cmd(void) {
    fprintf(stderr, "[cd] cmd %02x %02x %02x %02x %02x %02x %02x\n", cd.cmd[0], cd.cmd[1], cd.cmd[2], cd.cmd[3], cd.cmd[4], cd.cmd[5], cd.cmd[6]);
    if (cd.q_count == CD_QLEN) { fprintf(stderr, "[cd] reply queue full\n"); return; }
    Reply *r = &cd.q[(cd.q_head + cd.q_count++) % CD_QLEN];
    r->len = 0; r->ready_at = arm.cycles + CD_REPLY_DELAY;
    cd.building = r;
    push(cd.cmd[0]);
    switch (cd.cmd[0]) {
    case 0x83:                       /* identify: mfg, device, rev, flags, size, status */
        push(0x00); push(0x10);      /* manufacturer: MEI */
        push(0x00); push(0x01);      /* device */
        push(0x00); push(0x01);      /* revision */
        push(0x00); push(0x00);      /* flags */
        push(0x00); push(0x00);      /* driver size (x4) */
        push(cd_status_byte());
        break;
    default:
        push(cd_status_byte());
        break;
    }
}

static uint32_t cd_poll(XbusDev *d) { (void)d; return head_ready() ? 0x10 : 0; }
static uint8_t cd_status(XbusDev *d) {
    (void)d;
    Reply *r = head_ready();
    if (!r) return 0;
    uint8_t b = r->b[cd.st_pos++];
    if (cd.st_pos >= r->len) { cd.st_pos = 0; cd.q_head = (cd.q_head + 1) % CD_QLEN; cd.q_count--; }
    return b;
}
static uint8_t cd_data(XbusDev *d) { (void)d; return 0; }
static void cd_cmd(XbusDev *d, uint8_t b) {
    (void)d;
    cd.cmd[cd.cmd_len++] = b;
    if (cd.cmd_len == 7) { exec_cmd(); cd.cmd_len = 0; }
}

void cdrom_init(void) {
    memset(&cd, 0, sizeof cd);
    cd.dev.poll = cd_poll; cd.dev.status_byte = cd_status; cd.dev.data_byte = cd_data; cd.dev.cmd_byte = cd_cmd;
    xbus_dev[0] = &cd.dev;
}
