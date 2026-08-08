#ifndef SCHEMA_BLKID_PT_H
#define SCHEMA_BLKID_PT_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static inline void bpt_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

static inline int bpt_read_at(const char *devnode, uint64_t off, void *buf, size_t len) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buf, len, (off_t)off);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static inline uint16_t bpt_le16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t bpt_le32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static inline uint64_t bpt_le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void bpt_guid_str(const unsigned char g[16], char *out) {
    /* fields 1-3 little-endian (4/2/2), fields 4-5 big-endian (2/6) */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static inline uint64_t bpt_sector_size(const char *disksys) {
    char b[64];
    if (pi_sysattr(disksys, "queue/logical_block_size", b, sizeof b) == 0) {
        long s = atol(b);
        if (s >= 512) return (uint64_t)s;
    }
    return 512;
}

static inline int bpt_gpt_disk_uuid(const char *devnode, uint64_t ssz, char *uuid_out) {
    unsigned char hdr[96];
    if (bpt_read_at(devnode, ssz, hdr, sizeof hdr) != 0) return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) return -1;
    bpt_guid_str(hdr + 56, uuid_out);
    return 0;
}

static inline int bpt_all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static inline int bpt_name_safe(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || (c && strchr("#+-.:=@_", c) != NULL);
}

/* UTF-16LE (72 bytes, NUL-terminated within) -> UTF-8 -> blkid_encode_string */
static inline void bpt_name_encode(const unsigned char *name16, char *out, size_t outsz) {
    size_t o = 0;
    for (int i = 0; i + 1 < 72; i += 2) {
        unsigned u = (unsigned)name16[i] | ((unsigned)name16[i + 1] << 8);
        if (u == 0) break;
        unsigned char utf8[3]; int ulen;
        if (u < 0x80)       { utf8[0] = (unsigned char)u; ulen = 1; }
        else if (u < 0x800) { utf8[0] = (unsigned char)(0xc0 | (u >> 6));
                              utf8[1] = (unsigned char)(0x80 | (u & 0x3f)); ulen = 2; }
        else                { utf8[0] = (unsigned char)(0xe0 | (u >> 12));
                              utf8[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3f));
                              utf8[2] = (unsigned char)(0x80 | (u & 0x3f)); ulen = 3; }
        for (int k = 0; k < ulen; k++) {
            unsigned char c = utf8[k];
            if (bpt_name_safe(c)) { if (o + 1 < outsz) out[o++] = (char)c; }
            else if (o + 4 < outsz) o += (size_t)snprintf(out + o, outsz - o, "\\x%02x", c);
        }
    }
    if (o < outsz) out[o] = '\0'; else if (outsz) out[outsz - 1] = '\0';
}

static inline int bpt_gpt_entry(const char *devnode, uint64_t ssz, unsigned n,
                                unsigned char ent[128]) {
    unsigned char hdr[96];
    if (bpt_read_at(devnode, ssz, hdr, sizeof hdr) != 0) return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) return -1;
    uint64_t entry_lba = bpt_le64(hdr + 72);
    uint32_t count     = bpt_le32(hdr + 80);
    uint32_t entsz     = bpt_le32(hdr + 84);
    if (n < 1 || n > count || entsz < 128) return -1;
    uint64_t off = entry_lba * ssz + (uint64_t)(n - 1) * entsz;
    return bpt_read_at(devnode, off, ent, 128);
}

static inline void bpt_emit_gpt_entry(const unsigned char ent[128], uint64_t ssz, unsigned n,
                                      const char *diskdev, struct uevent *out) {
    if (bpt_all_zero(ent, 16)) return;   /* unused slot */
    uint64_t scale = ssz / 512;
    uint64_t first = bpt_le64(ent + 32);
    uint64_t last  = bpt_le64(ent + 40);
    uint64_t attrs = bpt_le64(ent + 48);

    char s[64];
    bpt_emit(out, "ID_PART_ENTRY_SCHEME", "gpt");

    char name[256];
    bpt_name_encode(ent + 56, name, sizeof name);
    if (name[0]) bpt_emit(out, "ID_PART_ENTRY_NAME", name);

    bpt_guid_str(ent + 16, s); bpt_emit(out, "ID_PART_ENTRY_UUID", s);
    bpt_guid_str(ent + 0,  s); bpt_emit(out, "ID_PART_ENTRY_TYPE", s);

    if (attrs != 0) {
        snprintf(s, sizeof s, "0x%llx", (unsigned long long)attrs);
        bpt_emit(out, "ID_PART_ENTRY_FLAGS", s);
    }
    snprintf(s, sizeof s, "%u", n);                    bpt_emit(out, "ID_PART_ENTRY_NUMBER", s);
    snprintf(s, sizeof s, "%llu", (unsigned long long)(first * scale));
    bpt_emit(out, "ID_PART_ENTRY_OFFSET", s);
    snprintf(s, sizeof s, "%llu", (unsigned long long)((last - first + 1) * scale));
    bpt_emit(out, "ID_PART_ENTRY_SIZE", s);
    bpt_emit(out, "ID_PART_ENTRY_DISK", diskdev);
}

static inline int bpt_dos_disk_uuid(const char *devnode, char *uuid_out) {
    unsigned char mbr[512];
    if (bpt_read_at(devnode, 0, mbr, sizeof mbr) != 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) return -1;
    snprintf(uuid_out, 9, "%08x", bpt_le32(mbr + 440));
    return 0;
}

static inline int bpt_dos_entry(const char *devnode, unsigned n, unsigned char ent[16]) {
    if (n < 1 || n > 4) return -1;   /* primaries only; logical chain: see note */
    unsigned char mbr[512];
    if (bpt_read_at(devnode, 0, mbr, sizeof mbr) != 0) return -1;
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) return -1;
    memcpy(ent, mbr + 446 + (n - 1) * 16, 16);
    return 0;
}

static inline void bpt_emit_dos_entry(const unsigned char ent[16], unsigned n,
                                      const char *diskdev, struct uevent *out) {
    unsigned char type = ent[4];
    if (type == 0) return;   /* empty entry */
    char s[64];
    bpt_emit(out, "ID_PART_ENTRY_SCHEME", "dos");
    snprintf(s, sizeof s, "0x%02x", type);          bpt_emit(out, "ID_PART_ENTRY_TYPE", s);
    if (ent[0] == 0x80) bpt_emit(out, "ID_PART_ENTRY_FLAGS", "0x80");
    snprintf(s, sizeof s, "%u", n);                 bpt_emit(out, "ID_PART_ENTRY_NUMBER", s);
    snprintf(s, sizeof s, "%u", bpt_le32(ent + 8)); bpt_emit(out, "ID_PART_ENTRY_OFFSET", s);
    snprintf(s, sizeof s, "%u", bpt_le32(ent + 12));bpt_emit(out, "ID_PART_ENTRY_SIZE", s);
    bpt_emit(out, "ID_PART_ENTRY_DISK", diskdev);
}

static inline int blkid_pt_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    out->n = 0;
    char syspath[PATH_MAX];
    if ((size_t)snprintf(syspath, sizeof syspath, "%s%s", sysroot, devpath) >= sizeof syspath)
        return 0;

    char partbuf[64];
    int is_part = (pi_sysattr(syspath, "partition", partbuf, sizeof partbuf) == 0);

    if (!is_part) {
        uint64_t ssz = bpt_sector_size(syspath);
        char uuid[37];
        if (bpt_gpt_disk_uuid(devnode, ssz, uuid) == 0) {
            bpt_emit(out, "ID_PART_TABLE_TYPE", "gpt");
            bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        } else if (bpt_dos_disk_uuid(devnode, uuid) == 0) {
            bpt_emit(out, "ID_PART_TABLE_TYPE", "dos");
            bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        }
        return 0;
    }

    unsigned n = (unsigned)atoi(partbuf);

    char parentsys[PATH_MAX]; safe_copy(parentsys, syspath, sizeof parentsys);
    if (pi_parent(parentsys) != 0) return 0;
    uint64_t ssz = bpt_sector_size(parentsys);
    char pdev[64];
    if (pi_sysattr(parentsys, "dev", pdev, sizeof pdev) != 0) pdev[0] = '\0';

    /* parent devnode = same directory as devnode, basename = parent sysfs basename */
    char dir[PATH_MAX]; safe_copy(dir, devnode, sizeof dir);
    char parentnode[PATH_MAX];
    if (pi_parent(dir) == 0) {
        safe_copy(parentnode, dir, sizeof parentnode);
        safe_copy(parentnode + strlen(parentnode), "/", sizeof parentnode - strlen(parentnode));
        safe_copy(parentnode + strlen(parentnode), pi_base(parentsys), sizeof parentnode - strlen(parentnode));
    } else {
        safe_copy(parentnode, "/dev/", sizeof parentnode);
        safe_copy(parentnode + strlen(parentnode), pi_base(parentsys), sizeof parentnode - strlen(parentnode));
    }

    char uuid[37];
    if (bpt_gpt_disk_uuid(parentnode, ssz, uuid) == 0) {
        bpt_emit(out, "ID_PART_TABLE_TYPE", "gpt");
        bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        unsigned char ent[128];
        if (bpt_gpt_entry(parentnode, ssz, n, ent) == 0)
            bpt_emit_gpt_entry(ent, ssz, n, pdev, out);
    } else if (bpt_dos_disk_uuid(parentnode, uuid) == 0) {
        bpt_emit(out, "ID_PART_TABLE_TYPE", "dos");
        bpt_emit(out, "ID_PART_TABLE_UUID", uuid);
        unsigned char ent[16];
        if (bpt_dos_entry(parentnode, n, ent) == 0)
            bpt_emit_dos_entry(ent, n, pdev, out);
    }
    return 0;
}

#endif /* SCHEMA_BLKID_PT_H */
