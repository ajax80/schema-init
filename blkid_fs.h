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

#endif /* SCHEMA_BLKID_FS_H */
