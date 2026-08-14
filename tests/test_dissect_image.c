#include "dissect_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_table(void) {
    assert(!strcmp(dissect_designator_for_guid("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), "esp"));
    assert(!strcmp(dissect_designator_for_guid("bc13c2ff-59e6-4262-a352-b275fd6f7172"), "xbootldr"));
    assert(!strcmp(dissect_designator_for_guid("933ac7e1-2eb4-4f13-b844-0e14e2aef915"), "home"));
    assert(!strcmp(dissect_designator_for_guid("3b8f8425-20e0-4f3b-907f-1a25a76f98e8"), "srv"));
    assert(!strcmp(dissect_designator_for_guid("4d21b016-b534-45c2-a9fb-5c16e091fd2d"), "var"));
    assert(!strcmp(dissect_designator_for_guid("7ec6f557-3bc5-4aca-b293-16ef5df639d1"), "tmp"));
    assert(!strcmp(dissect_designator_for_guid("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"), "swap"));
    /* x86-64 root + usr */
    assert(!strcmp(dissect_designator_for_guid("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"), "root"));
    assert(!strcmp(dissect_designator_for_guid("8484680c-9521-48c6-9c11-b0720656f69e"), "usr"));
    assert(!strcmp(dissect_designator_for_guid("2c7357ed-ebd2-46d9-aec1-23d437ec2bf5"), "root-verity"));
    /* unmapped: linux generic data, ntfs, random */
    assert(dissect_designator_for_guid("0fc63daf-8483-4772-8e79-3d69d8477de4") == NULL);
    assert(dissect_designator_for_guid("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7") == NULL);
    printf("test_table OK\n");
}

#include <unistd.h>
/* copied from test_blkid_pt.c: minimal GPT, 512-byte sectors, entries @1024 */
static void mk_gpt(const char *path, const unsigned char dg[16],
                   const unsigned char *entries, size_t elen) {
    unsigned char buf[1024 + 512] = {0};
    unsigned char *hdr = buf + 512;
    memcpy(hdr, "EFI PART", 8); memcpy(hdr + 56, dg, 16);
    hdr[72] = 2; hdr[80] = 128; hdr[84] = 128;
    if (entries && elen) memcpy(buf + 1024, entries, elen);
    FILE *f = fopen(path, "wb"); assert(f);
    fwrite(buf, 1, sizeof buf, f); fclose(f);
}
/* build one 128-byte GPT entry with a given type GUID (mixed-endian on wire) */
static void mk_entry(unsigned char e[128], const unsigned char type_le[16]) {
    memset(e, 0, 128);
    memcpy(e + 0, type_le, 16);                 /* type GUID */
    memset(e + 16, 0x11, 16);                   /* unique GUID (non-zero => used) */
    e[32] = 0x00; e[33] = 0x08;                 /* first LBA = 2048 */
    e[40] = 0xff; e[41] = 0x0f;                 /* last LBA (arbitrary > first) */
}

static void test_probe(void) {
    /* two partitions: esp (c12a7328..) then xbootldr (bc13c2ff..), mixed-endian type */
    unsigned char esp[16]  = {0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    unsigned char xbl[16]  = {0xff,0xc2,0x13,0xbc,0xe6,0x59,0x62,0x42,0xa3,0x52,0xb2,0x75,0xfd,0x6f,0x71,0x72};
    unsigned char ents[256];
    mk_entry(ents +   0, esp);
    mk_entry(ents + 128, xbl);
    unsigned char dg[16] = {0};  dg[0]=0xaa;
    char img[] = "/tmp/dissectXXXXXX"; int fd = mkstemp(img); assert(fd>=0); close(fd);
    mk_gpt(img, dg, ents, sizeof ents);

    struct uevent out; out.n = 0;
    assert(dissect_probe_build("", "", img, &out) == 0);
    assert(!strcmp(uevent_get(&out, "ID_DISSECT_IMAGE"), "1"));
    assert(!strcmp(uevent_get(&out, "ID_DISSECT_PART1_DESIGNATOR"), "esp"));
    assert(!strcmp(uevent_get(&out, "ID_DISSECT_PART2_DESIGNATOR"), "xbootldr"));
    unlink(img);
    printf("test_probe OK\n");
}

static void test_copy(void) {
    struct uevent ev; ev.n = 0;
    bpt_emit(&ev, "ID_PART_ENTRY_NUMBER", "2");
    bpt_emit(&ev, "ID_DISSECT_IMAGE", "1");
    bpt_emit(&ev, "ID_DISSECT_PART1_DESIGNATOR", "esp");
    bpt_emit(&ev, "ID_DISSECT_PART2_DESIGNATOR", "xbootldr");
    assert(dissect_copy_build("", "", "/dev/nvme0n1p2", &ev) == 0);
    assert(!strcmp(uevent_get(&ev, "ID_DISSECT_PART_DESIGNATOR"), "xbootldr"));

    /* partition with no mapped designator in the list => no key set */
    struct uevent ev2; ev2.n = 0;
    bpt_emit(&ev2, "ID_PART_ENTRY_NUMBER", "5");
    bpt_emit(&ev2, "ID_DISSECT_PART1_DESIGNATOR", "esp");
    assert(dissect_copy_build("", "", "/dev/nvme0n1p5", &ev2) == 0);
    assert(uevent_get(&ev2, "ID_DISSECT_PART_DESIGNATOR") == NULL);
    printf("test_copy OK\n");
}

int main(void) { test_table(); test_probe(); test_copy(); printf("ALL dissect_image tests OK\n"); return 0; }
