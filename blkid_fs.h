#ifndef SCHEMA_BLKID_FS_H
#define SCHEMA_BLKID_FS_H

#include "blkid_pt.h"   /* bpt_read_at, bpt_le*, bpt_emit, bpt_all_zero, bpt_name_safe (+ path_id/schema-udev) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline void fs_uuid_straight(const unsigned char g[16], char *out) {
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static inline void fs_safe_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && in[i]; i++) {
        if (o + 1 >= outsz) break;
        unsigned char c = in[i];
        out[o++] = (c < 0x20 || c == 0x7f || c == '/' || c == '\\') ? '_' : (char)c;
    }
    out[o] = '\0';
}

static inline void fs_encode_bytes(const unsigned char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen && in[i]; i++) {
        unsigned char c = in[i];
        if (bpt_name_safe(c)) { if (o + 1 < outsz) out[o++] = (char)c; }
        else if (o + 4 < outsz) o += (size_t)snprintf(out + o, outsz - o, "\\x%02x", c);
    }
    out[o] = '\0';
}

static inline void fs_utf16_to_utf8(const unsigned char *in, size_t bytelen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytelen; i += 2) {
        unsigned u = (unsigned)in[i] | ((unsigned)in[i + 1] << 8);
        if (u == 0) break;
        if (u < 0x80)       { if (o + 1 < outsz) out[o++] = (char)u; }
        else if (u < 0x800) { if (o + 2 < outsz) { out[o++] = (char)(0xc0 | (u >> 6));
                                                   out[o++] = (char)(0x80 | (u & 0x3f)); } }
        else                { if (o + 3 < outsz) { out[o++] = (char)(0xe0 | (u >> 12));
                                                   out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
                                                   out[o++] = (char)(0x80 | (u & 0x3f)); } }
    }
    if (o < outsz) out[o] = '\0'; else if (outsz) out[outsz - 1] = '\0';
}

static inline void fs_emit_uuid(struct uevent *out, const char *uuid) {
    bpt_emit(out, "ID_FS_UUID", uuid);
    bpt_emit(out, "ID_FS_UUID_ENC", uuid);
}

static inline void fs_emit_label(struct uevent *out, const unsigned char *raw, size_t len) {
    size_t n = 0; while (n < len && raw[n]) n++;
    if (n == 0) return;
    char safe[256], enc[256];
    fs_safe_bytes(raw, n, safe, sizeof safe);
    fs_encode_bytes(raw, n, enc, sizeof enc);
    if (safe[0]) bpt_emit(out, "ID_FS_LABEL", safe);
    if (enc[0])  bpt_emit(out, "ID_FS_LABEL_ENC", enc);
}

static inline int fs_probe_ext(const char *dev, struct uevent *out) {
    unsigned char sb[264];
    if (bpt_read_at(dev, 1024, sb, sizeof sb) != 0) return -1;
    if (!(sb[56] == 0x53 && sb[57] == 0xef)) return -1;   /* 0xEF53 LE */

    uint32_t fc = bpt_le32(sb + 0x5C);   /* feature_compat   */
    uint32_t fi = bpt_le32(sb + 0x60);   /* feature_incompat */
    uint32_t frc = bpt_le32(sb + 0x64);  /* feature_ro_compat */
    const char *type;
    /* ext4 markers: EXTENTS|64BIT|FLEX_BG (incompat) or HUGE_FILE|GDT_CSUM|DIR_NLINK|EXTRA_ISIZE|METADATA_CSUM (ro) */
    if ((fi & (0x0040u | 0x0080u | 0x0200u)) ||
        (frc & (0x0008u | 0x0010u | 0x0020u | 0x0040u | 0x0400u)))
        type = "ext4";
    else if (fc & 0x0004u)   /* HAS_JOURNAL */
        type = "ext3";
    else
        type = "ext2";

    bpt_emit(out, "ID_FS_TYPE", type);
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[37]; fs_uuid_straight(sb + 104, u); fs_emit_uuid(out, u);
    fs_emit_label(out, sb + 120, 16);
    char ver[16];
    snprintf(ver, sizeof ver, "%u.%u",
             (unsigned)bpt_le32(sb + 0x4C), (unsigned)bpt_le16(sb + 0x7E));
    bpt_emit(out, "ID_FS_VERSION", ver);
    return 0;
}

static inline int fs_probe_btrfs(const char *dev, struct uevent *out) {
    unsigned char sb[576];
    if (bpt_read_at(dev, 0x10000, sb, sizeof sb) != 0) return -1;
    if (memcmp(sb + 64, "_BHRfS_M", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "btrfs");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[37]; fs_uuid_straight(sb + 32, u); fs_emit_uuid(out, u);
    if (!bpt_all_zero(sb + 267, 16)) {
        char s[37]; fs_uuid_straight(sb + 267, s);
        bpt_emit(out, "ID_FS_UUID_SUB", s);
        bpt_emit(out, "ID_FS_UUID_SUB_ENC", s);
    }
    fs_emit_label(out, sb + 299, 256);
    return 0;
}

static inline int fs_probe_vfat(const char *dev, struct uevent *out) {
    unsigned char bs[512];
    if (bpt_read_at(dev, 0, bs, sizeof bs) != 0) return -1;
    if (!(bs[510] == 0x55 && bs[511] == 0xaa)) return -1;
    if (memcmp(bs + 3, "NTFS", 4) == 0 || memcmp(bs + 3, "EXFAT", 5) == 0) return -1;
    unsigned bps = bpt_le16(bs + 11);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || bs[13] == 0) return -1;

    int is_fat32 = (bpt_le16(bs + 22) == 0);
    unsigned ser = is_fat32 ? 67 : 39;
    unsigned lbo = is_fat32 ? 71 : 43;

    bpt_emit(out, "ID_FS_TYPE", "vfat");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[16];
    snprintf(u, sizeof u, "%02X%02X-%02X%02X", bs[ser + 3], bs[ser + 2], bs[ser + 1], bs[ser]);
    fs_emit_uuid(out, u);
    if (memcmp(bs + lbo, "NO NAME    ", 11) != 0) {
        unsigned char lbl[12]; memcpy(lbl, bs + lbo, 11); lbl[11] = '\0';
        int end = 11; while (end > 0 && lbl[end - 1] == ' ') end--;
        lbl[end] = '\0';
        if (end > 0) fs_emit_label(out, lbl, (size_t)end);
    }
    bpt_emit(out, "ID_FS_VERSION", is_fat32 ? "FAT32" : "FAT16");   /* FAT12 vs 16: see note */
    return 0;
}

static inline int fs_probe_swap(const char *dev, struct uevent *out) {
    static const uint64_t pages[] = {4096, 65536, 16384, 8192, 2048, 1024};
    int found = 0;
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        unsigned char m[10];
        if (bpt_read_at(dev, pages[i] - 10, m, 10) != 0) continue;
        if (memcmp(m, "SWAPSPACE2", 10) == 0 || memcmp(m, "SWAP-SPACE", 10) == 0) { found = 1; break; }
    }
    if (!found) return -1;
    unsigned char hdr[64];
    if (bpt_read_at(dev, 1024, hdr, sizeof hdr) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "swap");
    bpt_emit(out, "ID_FS_USAGE", "other");
    if (!bpt_all_zero(hdr + 12, 16)) {
        char u[37]; fs_uuid_straight(hdr + 12, u); fs_emit_uuid(out, u);
    }
    fs_emit_label(out, hdr + 28, 16);   /* volume_name @ 1024+28 = 1052 */
    char ver[16]; snprintf(ver, sizeof ver, "%u", (unsigned)bpt_le32(hdr));
    bpt_emit(out, "ID_FS_VERSION", ver);
    return 0;
}

static inline int fs_probe_ntfs(const char *dev, struct uevent *out) {
    unsigned char bs[512];
    if (bpt_read_at(dev, 0, bs, sizeof bs) != 0) return -1;
    if (memcmp(bs + 3, "NTFS    ", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "ntfs");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    char u[17];
    snprintf(u, sizeof u, "%02X%02X%02X%02X%02X%02X%02X%02X",
             bs[79], bs[78], bs[77], bs[76], bs[75], bs[74], bs[73], bs[72]);
    fs_emit_uuid(out, u);

    unsigned bps = bpt_le16(bs + 11);
    unsigned spc = bs[13];
    if (bps == 0 || spc == 0) return 0;
    uint64_t cluster = (uint64_t)bps * spc;
    int64_t mft_lcn = (int64_t)bpt_le64(bs + 48);
    int8_t rd = (int8_t)bs[64];
    uint64_t rec_size = (rd < 0) ? ((uint64_t)1 << (unsigned)(-rd)) : (uint64_t)rd * cluster;
    if (mft_lcn <= 0 || rec_size < 42 || rec_size > 65536) return 0;

    unsigned char *rec = (unsigned char *)malloc(rec_size);
    if (!rec) return 0;
    if (bpt_read_at(dev, (uint64_t)mft_lcn * cluster + 3 * rec_size, rec, rec_size) != 0 ||
        memcmp(rec, "FILE", 4) != 0) { free(rec); return 0; }

    /* apply the update-sequence-array fixup */
    unsigned usa_off = bpt_le16(rec + 4), usa_cnt = bpt_le16(rec + 6);
    if (usa_off && usa_cnt > 1 && bps) {
        for (unsigned i = 1; i < usa_cnt; i++) {
            uint64_t pos = (uint64_t)i * bps - 2;
            if (pos + 2 <= rec_size && usa_off + i * 2 + 1 < rec_size) {
                rec[pos]     = rec[usa_off + i * 2];
                rec[pos + 1] = rec[usa_off + i * 2 + 1];
            }
        }
    }

    unsigned ao = bpt_le16(rec + 20);
    while (ao + 8 <= rec_size) {
        uint32_t atype = bpt_le32(rec + ao);
        if (atype == 0xffffffffu) break;
        uint32_t alen = bpt_le32(rec + ao + 4);
        if (alen == 0 || (uint64_t)ao + alen > rec_size) break;
        if (atype == 0x60) {   /* $VOLUME_NAME, resident */
            uint32_t vlen = bpt_le32(rec + ao + 16);
            uint32_t voff = bpt_le16(rec + ao + 20);
            if (vlen > 0 && (uint64_t)ao + voff + vlen <= rec_size) {
                char lbl[256];
                fs_utf16_to_utf8(rec + ao + voff, vlen, lbl, sizeof lbl);
                fs_emit_label(out, (unsigned char *)lbl, strlen(lbl));
            }
            break;
        }
        ao += alen;
    }
    free(rec);
    return 0;
}

static inline int fs_probe_exfat(const char *dev, struct uevent *out) {
    unsigned char vbr[512];
    if (bpt_read_at(dev, 0, vbr, sizeof vbr) != 0) return -1;
    if (memcmp(vbr + 3, "EXFAT   ", 8) != 0) return -1;

    bpt_emit(out, "ID_FS_TYPE", "exfat");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");
    uint32_t serial = bpt_le32(vbr + 100);
    char u[16];
    snprintf(u, sizeof u, "%04X-%04X", (unsigned)(serial >> 16), (unsigned)(serial & 0xffff));
    fs_emit_uuid(out, u);
    /* exFAT LABEL lives in a root-directory volume-label entry (cluster walk) — deferred;
       port from libblkid superblocks/exfat.c if a real exFAT volume ever needs it. */
    return 0;
}

#endif /* SCHEMA_BLKID_FS_H */
