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

int main(void) {
    test_guid();
    test_le();
    printf("ALL blkid_pt tests passed\n");
    return 0;
}
