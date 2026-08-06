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

int main(void) {
    test_guid();
    test_le();
    test_gpt_disk();
    printf("ALL blkid_pt tests passed\n");
    return 0;
}
