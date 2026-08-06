#include "blkid_pt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_guid(void) {
    /* verified nvme0n1 disk GUID bytes -> 56d46da6-c484-4dd7-a6c3-d4693c92f94d */
    unsigned char g[16] = {0xa6,0x6d,0xd4,0x56, 0x84,0xc4, 0xd7,0x4d,
                           0xa6,0xc3, 0xd4,0x69,0x3c,0x92,0xf9,0x4d};
    char s[37];
    bpt_guid_str(g, s);
    assert(strcmp(s, "56d46da6-c484-4dd7-a6c3-d4693c92f94d") == 0);

    /* verified nvme0n1p1 type GUID -> c12a7328-f81f-11d2-ba4b-00a0c93ec93b */
    unsigned char t[16] = {0x28,0x73,0x2a,0xc1, 0x1f,0xf8, 0xd2,0x11,
                           0xba,0x4b, 0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    bpt_guid_str(t, s);
    assert(strcmp(s, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") == 0);

    printf("test_guid OK\n");
}

static void test_le(void) {
    unsigned char p[8] = {0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00};
    assert(bpt_le64(p) == 0x800);          /* 2048 */
    assert(bpt_le32(p) == 0x800);
    unsigned char q[8] = {0xff,0xc7,0x12,0x00,0x00,0x00,0x00,0x00};
    assert(bpt_le64(q) == 0x12c7ff);       /* 1230847 */
    printf("test_le OK\n");
}

#include <unistd.h>

/* write a minimal GPT: LBA1 header (@512), entries @1024. 512-byte sectors. */
static void mk_gpt(const char *path,
                   const unsigned char disk_guid[16],
                   const unsigned char *entries, size_t entries_len) {
    unsigned char buf[1024 + 512] = {0};
    unsigned char *hdr = buf + 512;
    memcpy(hdr, "EFI PART", 8);
    memcpy(hdr + 56, disk_guid, 16);
    /* partition_entry_lba = 2 */ hdr[72] = 2;
    /* num_entries = 128 */       hdr[80] = 128;
    /* entry_size = 128 */        hdr[84] = 128;
    if (entries && entries_len) memcpy(buf + 1024, entries, entries_len);
    FILE *f = fopen(path, "wb"); assert(f);
    fwrite(buf, 1, sizeof buf, f); fclose(f);
}

static void test_gpt_disk(void) {
    char img[] = "/tmp/bptgptXXXXXX";
    int fd = mkstemp(img); assert(fd >= 0); close(fd);
    unsigned char dg[16] = {0xa6,0x6d,0xd4,0x56,0x84,0xc4,0xd7,0x4d,
                            0xa6,0xc3,0xd4,0x69,0x3c,0x92,0xf9,0x4d};
    mk_gpt(img, dg, NULL, 0);

    char uuid[37];
    assert(bpt_gpt_disk_uuid(img, 512, uuid) == 0);
    assert(strcmp(uuid, "56d46da6-c484-4dd7-a6c3-d4693c92f94d") == 0);

    /* a non-GPT file fails */
    char bad[] = "/tmp/bptbadXXXXXX"; int bf = mkstemp(bad); assert(bf >= 0);
    { char z[1024] = {0}; assert(write(bf, z, sizeof z) == (ssize_t)sizeof z); } close(bf);
    assert(bpt_gpt_disk_uuid(bad, 512, uuid) != 0);

    unlink(img); unlink(bad);
    printf("test_gpt_disk OK\n");
}

static int bpt_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
static int bpt_absent(const struct uevent *e, const char *k) {
    return uevent_get(e, k) == NULL;
}

/* build the verified nvme0n1p1 entry (128 bytes) */
static void mk_entry_efi(unsigned char ent[128]) {
    memset(ent, 0, 128);
    unsigned char type[16] = {0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,
                              0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    unsigned char uuid[16] = {0x88,0x4a,0xa8,0x97,0xe4,0x34,0xde,0x4d,
                              0xb2,0xe8,0x4c,0x1e,0xe3,0xd5,0xfc,0xcc};
    memcpy(ent, type, 16); memcpy(ent + 16, uuid, 16);
    ent[32] = 0x00; ent[33] = 0x08;                 /* first_lba = 0x800 = 2048 */
    ent[40] = 0xff; ent[41] = 0xc7; ent[42] = 0x12; /* last_lba = 0x12c7ff = 1230847 */
    /* name "EFI System Partition" UTF-16LE */
    const char *nm = "EFI System Partition";
    for (size_t i = 0; nm[i]; i++) ent[56 + i*2] = (unsigned char)nm[i];
}

static void test_gpt_entry(void) {
    unsigned char ent[128];
    struct uevent e; e.n = 0;
    mk_entry_efi(ent);
    bpt_emit_gpt_entry(ent, 512, 1, "8:0", &e);
    assert(bpt_has(&e, "ID_PART_ENTRY_SCHEME", "gpt"));
    assert(bpt_has(&e, "ID_PART_ENTRY_TYPE", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"));
    assert(bpt_has(&e, "ID_PART_ENTRY_UUID", "97a84a88-34e4-4dde-b2e8-4c1ee3d5fccc"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NAME", "EFI\\x20System\\x20Partition"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NUMBER", "1"));
    assert(bpt_has(&e, "ID_PART_ENTRY_OFFSET", "2048"));
    assert(bpt_has(&e, "ID_PART_ENTRY_SIZE", "1228800"));   /* 1230847-2048+1 */
    assert(bpt_has(&e, "ID_PART_ENTRY_DISK", "8:0"));
    assert(bpt_absent(&e, "ID_PART_ENTRY_FLAGS"));          /* attrs 0 */

    /* flags set, no name */
    unsigned char ent2[128]; memcpy(ent2, ent, 128);
    memset(ent2 + 56, 0, 72);                               /* clear name */
    ent2[48+7] = 0x80;                                      /* attrs = 0x8000000000000000 */
    struct uevent e2; e2.n = 0;
    bpt_emit_gpt_entry(ent2, 512, 2, "8:16", &e2);
    assert(bpt_absent(&e2, "ID_PART_ENTRY_NAME"));
    assert(bpt_has(&e2, "ID_PART_ENTRY_FLAGS", "0x8000000000000000"));
    assert(bpt_has(&e2, "ID_PART_ENTRY_NUMBER", "2"));

    /* empty slot (all-zero type) -> emits nothing */
    unsigned char ent3[128]; memset(ent3, 0, 128);
    struct uevent e3; e3.n = 0;
    bpt_emit_gpt_entry(ent3, 512, 3, "8:16", &e3);
    assert(e3.n == 0);

    /* 4Kn scaling: sector_size 4096 -> offset/size ×8 */
    struct uevent e4; e4.n = 0;
    bpt_emit_gpt_entry(ent, 4096, 1, "8:0", &e4);
    assert(bpt_has(&e4, "ID_PART_ENTRY_OFFSET", "16384"));  /* 2048*8 */
    assert(bpt_has(&e4, "ID_PART_ENTRY_SIZE", "9830400"));  /* 1228800*8 */

    printf("test_gpt_entry OK\n");
}

static void test_dos(void) {
    char img[] = "/tmp/bptdosXXXXXX";
    int fd = mkstemp(img); assert(fd >= 0);
    unsigned char buf[512] = {0};
    /* disk signature at 440 = 0x11223344 (LE bytes) */
    buf[440] = 0x44; buf[441] = 0x33; buf[442] = 0x22; buf[443] = 0x11;
    /* primary entry 1 @446: bootable, type 0x83 (linux), start 2048, size 1000 */
    unsigned char *p = buf + 446;
    p[0] = 0x80;                    /* bootable */
    p[4] = 0x83;                    /* type */
    p[8] = 0x00; p[9] = 0x08;       /* start_lba = 2048 */
    p[12] = 0xe8; p[13] = 0x03;     /* num_sectors = 1000 */
    buf[510] = 0x55; buf[511] = 0xaa;
    assert(write(fd, buf, sizeof buf) == (ssize_t)sizeof buf); close(fd);

    char uuid[9];
    assert(bpt_dos_disk_uuid(img, uuid) == 0);
    assert(strcmp(uuid, "11223344") == 0);

    unsigned char ent[16];
    assert(bpt_dos_entry(img, 1, ent) == 0);
    struct uevent e; e.n = 0;
    bpt_emit_dos_entry(ent, 1, "8:0", &e);
    assert(bpt_has(&e, "ID_PART_ENTRY_SCHEME", "dos"));
    assert(bpt_has(&e, "ID_PART_ENTRY_TYPE", "0x83"));
    assert(bpt_has(&e, "ID_PART_ENTRY_NUMBER", "1"));
    assert(bpt_has(&e, "ID_PART_ENTRY_OFFSET", "2048"));
    assert(bpt_has(&e, "ID_PART_ENTRY_SIZE", "1000"));
    assert(bpt_has(&e, "ID_PART_ENTRY_FLAGS", "0x80"));

    unlink(img);
    printf("test_dos OK\n");
}

int main(void) {
    test_guid();
    test_le();
    test_gpt_disk();
    test_gpt_entry();
    test_dos();
    printf("ALL blkid_pt tests passed\n");
    return 0;
}
