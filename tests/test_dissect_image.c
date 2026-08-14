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

int main(void) { test_table(); printf("ALL dissect_image tests OK\n"); return 0; }
