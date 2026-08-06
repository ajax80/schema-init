#include "blkid_fs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fs_has(const struct uevent *e, const char *k, const char *v) {
    const char *g = uevent_get(e, k); return g && strcmp(g, v) == 0;
}
static int fs_absent(const struct uevent *e, const char *k) { return uevent_get(e, k) == NULL; }

static void test_helpers(void) {
    unsigned char g[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                           0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    char s[37]; fs_uuid_straight(g, s);
    assert(strcmp(s, "cf4f2b07-f150-404f-bde1-4d9f545594b4") == 0);

    /* encode: space -> \x20, alnum kept */
    char enc[64]; fs_encode_bytes((const unsigned char *)"My Disk", 7, enc, sizeof enc);
    assert(strcmp(enc, "My\\x20Disk") == 0);
    /* safe: space kept, '/' -> _ */
    char safe[64]; fs_safe_bytes((const unsigned char *)"a/b c", 5, safe, sizeof safe);
    assert(strcmp(safe, "a_b c") == 0);

    /* utf16 -> utf8 (ASCII "RECOVERY") */
    unsigned char u16[16] = {'R',0,'E',0,'C',0,'O',0,'V',0,'E',0,'R',0,'Y',0};
    char u8[32]; fs_utf16_to_utf8(u16, 16, u8, sizeof u8);
    assert(strcmp(u8, "RECOVERY") == 0);

    /* emit label */
    struct uevent e; e.n = 0;
    fs_emit_label(&e, (const unsigned char *)"fedora", 6);
    assert(fs_has(&e, "ID_FS_LABEL", "fedora") && fs_has(&e, "ID_FS_LABEL_ENC", "fedora"));
    struct uevent e2; e2.n = 0;
    fs_emit_label(&e2, (const unsigned char *)"", 0);
    assert(fs_absent(&e2, "ID_FS_LABEL"));

    printf("test_helpers OK\n");
}

int main(void) {
    test_helpers();
    printf("ALL blkid_fs tests passed\n");
    return 0;
}
