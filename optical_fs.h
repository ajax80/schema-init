#ifndef OPTICAL_FS_H
#define OPTICAL_FS_H

#include "schema-udev.h"
#include "blkid_fs.h"     /* bpt_read_at, bpt_emit, fs_emit_label/uuid, fs_safe/encode_bytes */
#include <string.h>
#include <stdint.h>
#include <stdio.h>

static inline void fs_trim_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t start = 0;
    while (start < inlen && (in[start] == ' ' || in[start] == '\t' || in[start] == '\r' || in[start] == '\n')) start++;
    size_t end = inlen;
    while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r' || in[end - 1] == '\n')) end--;
    if (end > start) fs_safe_bytes(in + start, end - start, out, outsz);
    else out[0] = '\0';
}

static inline void fs_trim_encode_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t start = 0;
    while (start < inlen && (in[start] == ' ' || in[start] == '\t' || in[start] == '\r' || in[start] == '\n')) start++;
    size_t end = inlen;
    while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r' || in[end - 1] == '\n')) end--;
    if (end > start) fs_encode_bytes(in + start, end - start, out, outsz);
    else out[0] = '\0';
}

static inline int fs_probe_iso9660(const char *dev, struct uevent *out) {
    unsigned char pvd[2048];
    if (bpt_read_at(dev, 32768, pvd, sizeof pvd) != 0) return -1;
    if (pvd[0] != 0x01 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "iso9660");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");

    char sysid[64];
    fs_trim_bytes(pvd + 8, 32, sysid, sizeof sysid);
    if (sysid[0]) bpt_emit(out, "ID_FS_SYSTEM_ID", sysid);

    unsigned char lbl[33]; memcpy(lbl, pvd + 40, 32);
    int lend = 32; while (lend > 0 && (lbl[lend - 1] == ' ' || lbl[lend - 1] == '\0')) lend--;
    if (lend > 0) fs_emit_label(out, lbl, (size_t)lend);

    char appid[256];
    fs_trim_encode_bytes(pvd + 566, 128, appid, sizeof appid);
    if (appid[0]) bpt_emit(out, "ID_FS_APPLICATION_ID", appid);

    if (memcmp(pvd + 813, "0000000000000000", 16) != 0) {
        char uuid[32];
        snprintf(uuid, sizeof uuid, "%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s",
                 pvd + 813, pvd + 817, pvd + 819, pvd + 821, pvd + 823, pvd + 825, pvd + 827);
        fs_emit_uuid(out, uuid);
    }

    unsigned char svd[2048];
    for (int sec = 17; sec < 32; sec++) {
        if (bpt_read_at(dev, (uint64_t)sec * 2048, svd, sizeof svd) != 0) break;
        if (svd[0] == 0xff) break;
        if (svd[0] == 0x02 && memcmp(svd + 1, "CD001", 5) == 0) {
            bpt_emit(out, "ID_FS_VERSION", "Joliet Extension");
            break;
        }
    }

    return 0;
}

/* UDF dstring: field[0]=compression id, field[fieldlen-1]=length (incl comp byte);
   comp 8 = 8-bit chars, comp 16 = UTF-16BE, both starting at field[1]. */
static inline void udf_dstring(const unsigned char *f, int fieldlen, char *out, int outsz) {
    int len = f[fieldlen - 1], o = 0;
    if (len <= 1) { out[0] = 0; return; }
    if (f[0] == 16) { for (int i = 1; i + 1 < len && o < outsz - 1; i += 2) out[o++] = f[i + 1]; }
    else            { for (int i = 1; i < len && o < outsz - 1; i++)     out[o++] = f[i]; }
    out[o] = 0;
}

static inline int fs_probe_udf(const char *dev, struct uevent *out) {
    unsigned char d[2048];
    int found = 0;
    for (int sec = 16; sec <= 20; sec++) {
        if (bpt_read_at(dev, (uint64_t)sec * 2048, d, sizeof d) != 0) break;
        if (memcmp(d + 1, "NSR02", 5) == 0 || memcmp(d + 1, "NSR03", 5) == 0) { found = 1; break; }
        if (memcmp(d + 1, "TEA01", 5) == 0) break;      /* end of VRS */
    }
    if (!found) return -1;
    bpt_emit(out, "ID_FS_TYPE", "udf");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");

    if (bpt_read_at(dev, 256ULL * 2048, d, sizeof d) != 0) return 0;   /* AVDP */
    if ((d[0] | (d[1] << 8)) != 2) return 0;
    uint32_t loc = d[20] | (d[21]<<8) | (d[22]<<16) | ((uint32_t)d[23]<<24);
    uint32_t mlen = d[16] | (d[17]<<8) | (d[18]<<16) | ((uint32_t)d[19]<<24);
    uint32_t nsec = mlen / 2048; if (nsec > 64) nsec = 64;

    char label[128]="", volid[64]="", volset[128]="", appid[128]="", version[8]="";
    for (uint32_t i = 0; i <= nsec; i++) {
        if (bpt_read_at(dev, (uint64_t)(loc + i) * 2048, d, sizeof d) != 0) break;
        unsigned t = d[0] | (d[1] << 8);
        if (t == 1) {                                    /* PVD */
            udf_dstring(d + 24, 32, volid, sizeof volid);
            udf_dstring(d + 72, 128, volset, sizeof volset);
            const unsigned char *impl = d + 388 + 1;     /* ImplId identifier (skip flags) */
            size_t ilen = strnlen((const char *)impl, 23);
            if (ilen && impl[0] == '*') { impl++; ilen--; }
            fs_encode_bytes(impl, ilen, appid, sizeof appid);
        } else if (t == 6) {                             /* LVD */
            udf_dstring(d + 84, 128, label, sizeof label);
            unsigned rev = d[240] | (d[241] << 8);
            snprintf(version, sizeof version, "%x.%02x", rev >> 8, rev & 0xff);
        } else if (t == 8) break;                        /* terminating descriptor */
    }
    if (label[0])  { fs_emit_label(out, (const unsigned char *)label, strlen(label));
                     bpt_emit(out, "ID_FS_LOGICAL_VOLUME_ID", label); }
    if (volid[0])  bpt_emit(out, "ID_FS_VOLUME_ID", volid);
    if (volset[0]) bpt_emit(out, "ID_FS_VOLUME_SET_ID", volset);
    if (version[0] && strcmp(version, "0.00") != 0) bpt_emit(out, "ID_FS_VERSION", version);
    if (appid[0])  bpt_emit(out, "ID_FS_APPLICATION_ID", appid);
    if (volset[0]) {                                      /* UUID: lowercase volset, pad to 16 */
        char uuid[17]; int j;
        for (j = 0; j < 16 && volset[j]; j++) {
            char c = volset[j];
            uuid[j] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        for (; j < 16; j++) uuid[j] = '0';
        uuid[16] = 0;
        fs_emit_uuid(out, uuid);
    }
    return 0;
}

static inline int optical_fs_probe(const char *devnode, struct uevent *out) {
    if (fs_probe_udf(devnode, out) == 0) return 0;       /* UDF wins on bridge discs */
    if (fs_probe_iso9660(devnode, out) == 0) return 0;
    return -1;
}

#endif /* OPTICAL_FS_H */
