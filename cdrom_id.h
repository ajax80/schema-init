#ifndef CDROM_ID_H
#define CDROM_ID_H

#include "schema-udev.h"
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static inline int cdrom_id_decode(const uint8_t *buf, int len, struct uevent *out) {
    out->n = 0;
    if (len < 8) return 0;
    #define CEMIT(k) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out, (k))) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], "1", UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    int off = 8;                                  /* skip 8-byte header */
    while (off + 4 <= len) {
        unsigned feat = ((unsigned)buf[off] << 8) | buf[off + 1];
        int addlen = buf[off + 3];
        if (off + 4 + addlen > len) break;
        if (feat == 0x0000) {                     /* Profile List */
            CEMIT("ID_CDROM");
            const uint8_t *fd = buf + off + 4;
            for (int p = 0; p + 4 <= addlen; p += 4) {
                unsigned profile = ((unsigned)fd[p] << 8) | fd[p + 1];
                switch (profile) {
                case 0x02: CEMIT("ID_CDROM_RW_REMOVABLE"); break;
                case 0x08: CEMIT("ID_CDROM_CD"); break;
                case 0x09: CEMIT("ID_CDROM_CD_R"); break;
                case 0x0a: CEMIT("ID_CDROM_CD_RW"); break;
                case 0x10: CEMIT("ID_CDROM_DVD"); break;
                case 0x11: CEMIT("ID_CDROM_DVD_R"); break;
                case 0x12: CEMIT("ID_CDROM_DVD_RAM"); break;
                case 0x13: CEMIT("ID_CDROM_DVD_RW"); CEMIT("ID_CDROM_DVD_RW_RO"); break;
                case 0x14: CEMIT("ID_CDROM_DVD_RW"); CEMIT("ID_CDROM_DVD_RW_SEQ"); break;
                case 0x15: CEMIT("ID_CDROM_DVD_R_DL"); CEMIT("ID_CDROM_DVD_R_DL_SEQ"); break;
                case 0x16: CEMIT("ID_CDROM_DVD_R_DL"); CEMIT("ID_CDROM_DVD_R_DL_JR"); break;
                case 0x1a: CEMIT("ID_CDROM_DVD_PLUS_RW"); break;
                case 0x1b: CEMIT("ID_CDROM_DVD_PLUS_R"); break;
                case 0x2a: CEMIT("ID_CDROM_DVD_PLUS_RW_DL"); break;
                case 0x2b: CEMIT("ID_CDROM_DVD_PLUS_R_DL"); break;
                case 0x40: CEMIT("ID_CDROM_BD"); break;
                case 0x41: case 0x42: CEMIT("ID_CDROM_BD_R"); break;
                case 0x43: CEMIT("ID_CDROM_BD_RE"); break;
                case 0x50: CEMIT("ID_CDROM_HDDVD"); break;
                case 0x51: CEMIT("ID_CDROM_HDDVD_R"); break;
                case 0x52: CEMIT("ID_CDROM_HDDVD_RW"); break;
                default: break;
                }
            }
        }
        off += 4 + addlen;
    }
    #undef CEMIT
    return out->n;
}

static inline int cdrom_media_type(const uint8_t *buf, int len, struct uevent *out) {
    if (len < 8) return 0;
    unsigned cur = ((unsigned)buf[6] << 8) | buf[7];
    if (cur == 0x0000 || cur == 0xffff) return 0;
    #define MEMIT(k) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out, (k))) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], "1", UE_VAL_MAX); out->n++; } \
    } while (0)
    MEMIT("ID_CDROM_MEDIA");
    switch (cur) {
    case 0x08: MEMIT("ID_CDROM_MEDIA_CD"); break;
    case 0x09: MEMIT("ID_CDROM_MEDIA_CD_R"); break;
    case 0x0a: MEMIT("ID_CDROM_MEDIA_CD_RW"); break;
    case 0x10: MEMIT("ID_CDROM_MEDIA_DVD"); break;
    case 0x11: MEMIT("ID_CDROM_MEDIA_DVD_R"); break;
    case 0x12: MEMIT("ID_CDROM_MEDIA_DVD_RAM"); break;
    case 0x13: MEMIT("ID_CDROM_MEDIA_DVD_RW_RO"); break;
    case 0x14: MEMIT("ID_CDROM_MEDIA_DVD_RW_SEQ"); break;
    case 0x15: MEMIT("ID_CDROM_MEDIA_DVD_R_DL_SEQ"); break;
    case 0x16: MEMIT("ID_CDROM_MEDIA_DVD_R_DL_JR"); break;
    case 0x1a: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_RW"); break;
    case 0x1b: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_R"); break;
    case 0x2a: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_RW_DL"); break;
    case 0x2b: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_R_DL"); break;
    case 0x40: MEMIT("ID_CDROM_MEDIA_BD"); break;
    case 0x41: case 0x42: MEMIT("ID_CDROM_MEDIA_BD_R"); break;
    case 0x43: MEMIT("ID_CDROM_MEDIA_BD_RE"); break;
    case 0x50: MEMIT("ID_CDROM_MEDIA_HDDVD"); break;
    case 0x51: MEMIT("ID_CDROM_MEDIA_HDDVD_R"); break;
    case 0x52: MEMIT("ID_CDROM_MEDIA_HDDVD_RW"); break;
    default: break;
    }
    #undef MEMIT
    return out->n;
}

static inline int cdrom_discinfo_decode(const uint8_t *di, int len, struct uevent *out) {
    if (len < 12) return 0;
    #define DEMIT(k,v) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out,(k))) { \
            safe_copy(out->key[out->n],(k),UE_KEY_MAX); \
            safe_copy(out->val[out->n],(v),UE_VAL_MAX); out->n++; } \
    } while (0)
    DEMIT("ID_CDROM_MEDIA", "1");
    const char *st = NULL;
    switch (di[2] & 3) { case 0: st="blank"; break; case 1: st="appendable"; break;
                         case 2: st="complete"; break; default: st=NULL; }
    if (st) DEMIT("ID_CDROM_MEDIA_STATE", st);
    char num[16];
    int sessions = (di[9] << 8) | di[4];
    snprintf(num, sizeof num, "%d", sessions); DEMIT("ID_CDROM_MEDIA_SESSION_COUNT", num);
    int first = di[3], last = (di[11] << 8) | di[6];
    int tracks = last - first + 1;
    if (tracks < 0) tracks = 0;
    snprintf(num, sizeof num, "%d", tracks); DEMIT("ID_CDROM_MEDIA_TRACK_COUNT", num);
    #undef DEMIT
    return out->n;
}

static inline int cdrom_get_config(const char *devnode, uint8_t *buf, size_t bufsz, int *len) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(buf, 0, bufsz);
    uint8_t cdb[10] = {0x46, 0x00, 0, 0, 0, 0, 0,
                       (uint8_t)(bufsz >> 8), (uint8_t)(bufsz & 0xff), 0};
    uint8_t sense[32] = {0};
    struct sg_io_hdr io = {0};
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.dxfer_len = (unsigned)bufsz;
    io.dxferp = buf;
    io.sbp = sense;
    io.mx_sb_len = sizeof sense;
    io.timeout = 5000;
    int rc = ioctl(fd, SG_IO, &io);
    close(fd);
    if (rc < 0 || (io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
    int datalen = ((int)buf[0] << 24) | ((int)buf[1] << 16) | ((int)buf[2] << 8) | buf[3];
    int total = datalen + 4;
    if (total > (int)bufsz) total = (int)bufsz;
    *len = total;
    return 0;
}

static inline int cdrom_id_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    uint8_t buf[2048];
    int len = 0;
    if (cdrom_get_config(devnode, buf, sizeof buf, &len) != 0) return 0;
    return cdrom_id_decode(buf, len, out);
}

#endif /* CDROM_ID_H */
