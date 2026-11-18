/* 3DODroid - CD-ROM drive on the XBUS (bring-up: logs commands) */
#include "sys3do.h"
#include "dbglog.h"
#include "xbus.h"
#include "disc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    /* data phase of READ (0x10): sectors streamed through the data FIFO */
    uint32_t rd_lba, rd_left;           /* next sector, sectors still to buffer */
    uint8_t sec[2048]; int sec_pos, sec_valid;
    uint64_t data_ready_at;
    uint8_t rd_echo;
} Cd;
static Cd cd;
static int attention;
static void load_next_sector(void);

static Reply *head_ready(void) {
    if (!cd.q_count) return NULL;
    Reply *r = &cd.q[cd.q_head];
    return arm.cycles >= r->ready_at ? r : NULL;
}

static Disc *cur_disc;

/* drive status byte (cdrom.h CDROM_STATUS_*): 0x80 door (closed), 0x40 disc
 * in, 0x20 spun up, 0x10 error, 0x02 double speed, 0x01 ready. The BIOS's MEI
 * dipir gives up unless the door bit is set. */
static uint8_t cd_status_byte(void) { return cur_disc ? 0x80 | 0x40 | 0x20 | 0x02 | 0x01 : 0x80; }

/* Inserting a disc raises attention and latches a dipir request for unit 0,
 * which is how the BIOS learns it should validate and boot the disc. */
void cdrom_insert(Disc *d) {
    cur_disc = d;
    if (d) { attention = 1; clio_dipir_request(0); }
}
Disc *cdrom_disc(void) { return cur_disc; }

static void push(uint8_t b) { Reply *r = cd.building; if (r && r->len < 64) r->b[r->len++] = b; }

static void exec_cmd(void) {
    if (xbus_log) fprintf(stderr, "[cd f%llu] cmd %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long long)sys.frame, cd.cmd[0], cd.cmd[1], cd.cmd[2], cd.cmd[3], cd.cmd[4], cd.cmd[5], cd.cmd[6]);
    if (cd.cmd[0] == 0x10)
        dbg_stream("cd: read %02u:%02u:%02u (sector %u) x%u", cd.cmd[1], cd.cmd[2], cd.cmd[3],
                   ((cd.cmd[1] * 60u + cd.cmd[2]) * 75u + cd.cmd[3]) - 150u, (unsigned)(cd.cmd[4] << 16 | cd.cmd[5] << 8 | cd.cmd[6]));
    else {
        uint32_t key = (uint32_t)cd.cmd[0] << 24 ^ (uint32_t)cd.cmd[1] << 16 ^ (uint32_t)cd.cmd[2] << 8 ^ cd.cmd[3] ^ (uint32_t)cd.cmd[6] << 4;
        if (dbg_once(13, key)) dbg("cd: cmd %02x %02x %02x %02x %02x %02x %02x", cd.cmd[0], cd.cmd[1], cd.cmd[2], cd.cmd[3], cd.cmd[4], cd.cmd[5], cd.cmd[6]);
        dbg_stream("cd: cmd %02x %02x %02x %02x %02x %02x %02x", cd.cmd[0], cd.cmd[1], cd.cmd[2], cd.cmd[3], cd.cmd[4], cd.cmd[5], cd.cmd[6]);
    }
    if (cd.cmd[0] == 0x10) {                    /* READ DATA: MSF, count in bytes 5-6 */
        uint32_t lba = ((uint32_t)cd.cmd[1] * 60 + cd.cmd[2]) * 75 + cd.cmd[3];
        lba = lba >= 150 ? lba - 150 : 0;
        uint32_t cnt = ((uint32_t)cd.cmd[4] << 16) | ((uint32_t)cd.cmd[5] << 8) | cd.cmd[6];
        cd.rd_lba = lba; cd.rd_left = cnt ? cnt : 1; cd.rd_echo = 0x10;
        cd.data_ready_at = arm.cycles + CD_REPLY_DELAY;     /* seek + first sector */
        load_next_sector();
        return;
    }
    if (cd.cmd[0] == 0x08) { cd.rd_left = 0; cd.sec_valid = 0; }   /* abort */
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
    case 0x8B: {                     /* disc info: type, first/last track, lead-out MSF */
        if (!cur_disc) { push(cd_status_byte()); break; }
        int n = disc_track_count(cur_disc);
        uint8_t m, sec, f; lba_to_msf(disc_total_sectors(cur_disc), &m, &sec, &f);
        push(0x00);                                  /* disc type: CD-ROM / CD-DA */
        push((uint8_t)disc_track(cur_disc, 0)->number);
        push((uint8_t)disc_track(cur_disc, n - 1)->number);
        push(m); push(sec); push(f);
        push(cd_status_byte());
        break;
    }
    case 0x8C: {                     /* TOC entry for track cmd[2] */
        const DiscTrack *t = NULL;
        if (cur_disc) for (int i = 0; i < disc_track_count(cur_disc); i++)
            if (disc_track(cur_disc, i)->number == cd.cmd[2]) t = disc_track(cur_disc, i);
        uint8_t m = 0, sec = 0, f = 0;
        if (t) lba_to_msf(t->start, &m, &sec, &f);
        /* data: reserved, ADR/control, track, reserved, M, S, F, reserved */
        push(0x00);
        push(t ? (t->type == TRK_AUDIO ? 0x10 : 0x14) : 0x00);   /* ADR 1, control: data track */
        push(t ? (uint8_t)t->number : 0);
        push(0x00);
        push(m); push(sec); push(f);
        push(0x00);
        push(t ? cd_status_byte() : (uint8_t)(cd_status_byte() | 0x10));
        break;
    }
    case 0x8D: {                     /* session info: valid flag, start of last session (MSF) */
        /* The BIOS reads the ISO volume descriptor at session start + 16 to
         * classify the disc; with the flag clear it uses a bogus base. */
        uint8_t m = 0, sec = 0, f = 0;
        if (cur_disc) lba_to_msf(disc_track(cur_disc, 0)->start, &m, &sec, &f);
        push(0x01); push(m); push(sec); push(f); push(0x00); push(0x00);
        push(cd_status_byte());
        break;
    }
    case 0x85: {                     /* capacity: end of disc MSF, block size */
        uint8_t m = 0, sec = 0, f = 0;
        if (cur_disc) lba_to_msf(disc_total_sectors(cur_disc), &m, &sec, &f);
        push(0x00); push(m); push(sec); push(f); push(0x08); push(0x00);
        push(cd_status_byte());
        break;
    }
    case 0x80:                       /* data path check: fixed test pattern */
        push(0xAA); push(0x55);
        push(cd_status_byte());
        break;
    default:
        if (cd.cmd[0] != 0x09 && cd.cmd[0] != 0x02 && cd.cmd[0] != 0x08) dbg("cd: command %02x not emulated (status only)", cd.cmd[0]);
        push(cd_status_byte());
        break;
    }
}

static int data_avail(void) { return cd.sec_valid && arm.cycles >= cd.data_ready_at; }

/* Attention (poll bit 0x80): a newly inserted disc. Cleared when the host
 * acknowledges by writing 0x80 to the poll register. */

static uint32_t cd_poll(XbusDev *d) {
    (void)d;
    return (head_ready() ? 0x10 : 0) | (data_avail() ? 0x20 : 0) | (attention ? 0x80 : 0);
}

void cdrom_ack_attention(void) { attention = 0; }

static void load_next_sector(void) {
    cd.sec_valid = 0; cd.sec_pos = 0;
    if (!cd.rd_left || !cur_disc) return;
    disc_read_data(cur_disc, cd.rd_lba, cd.sec);
    cd.rd_lba++; cd.rd_left--;
    cd.sec_valid = 1;
}

/* Status for a READ only arrives once all its data has been taken. */
static void read_finished(void) {
    Reply *r = &cd.q[(cd.q_head + cd.q_count++) % CD_QLEN];
    r->len = 0; r->ready_at = arm.cycles + 2000;
    cd.building = r;
    push(cd.rd_echo); push(cd_status_byte());
}

uint8_t cdrom_data_byte(void) {
    if (!cd.sec_valid) return 0;
    uint8_t b = cd.sec[cd.sec_pos++];
    if (cd.sec_pos >= 2048) {
        load_next_sector();
        if (!cd.sec_valid) read_finished();
    }
    return b;
}
static uint8_t cd_status(XbusDev *d) {
    (void)d;
    Reply *r = head_ready();
    if (!r) return 0;
    uint8_t b = r->b[cd.st_pos++];
    if (cd.st_pos >= r->len) { cd.st_pos = 0; cd.q_head = (cd.q_head + 1) % CD_QLEN; cd.q_count--; }
    return b;
}
static uint8_t cd_data(XbusDev *d) { (void)d; return cdrom_data_byte(); }
static void cd_cmd(XbusDev *d, uint8_t b) {
    (void)d;
    cd.cmd[cd.cmd_len++] = b;
    int need = cd.cmd[0] == 0x08 ? 1 : 7;      /* abort is a single byte */
    if (cd.cmd_len == need) { for (int i = need; i < 7; i++) cd.cmd[i] = 0; exec_cmd(); cd.cmd_len = 0; }
}

void cdrom_init(void) {
    memset(&cd, 0, sizeof cd);
    cd.dev.poll = cd_poll; cd.dev.status_byte = cd_status; cd.dev.data_byte = cd_data; cd.dev.cmd_byte = cd_cmd;
    xbus_dev[0] = &cd.dev;
}
