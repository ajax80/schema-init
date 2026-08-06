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

#endif /* SCHEMA_BLKID_FS_H */
