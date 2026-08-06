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

#include <stdint.h>

static void wr_img(const char *path, uint64_t off, const unsigned char *buf, size_t len) {
    FILE *f = fopen(path, "r+b"); if (!f) f = fopen(path, "w+b"); assert(f);
    fseek(f, (long)off, SEEK_SET); fwrite(buf, 1, len, f); fclose(f);
}
static void zero_img(const char *path, size_t size) {
    FILE *f = fopen(path, "wb"); assert(f);
    unsigned char z[512] = {0};
    for (size_t i = 0; i < size; i += sizeof z) fwrite(z, 1, sizeof z, f);
    fclose(f);
}

static void test_ext(void) {
    char img[] = "/tmp/fsextXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 4096);
    unsigned char sb[264] = {0};
    sb[56] = 0x53; sb[57] = 0xef;                         /* magic 0xEF53 */
    unsigned char uu[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                            0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    memcpy(sb + 104, uu, 16);                             /* s_uuid */
    /* no label (sb+120 stays zero) */
    sb[0x4C] = 1;                                         /* s_rev_level = 1 */
    /* s_minor_rev_level (u16 @ 0x7E) = 0 */
    sb[0x60] = 0x40;                                      /* incompat EXTENTS -> ext4 */
    wr_img(img, 1024, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_ext(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ext4"));
    assert(fs_has(&e, "ID_FS_USAGE", "filesystem"));
    assert(fs_has(&e, "ID_FS_UUID", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_UUID_ENC", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_VERSION", "1.0"));
    assert(fs_absent(&e, "ID_FS_LABEL"));

    unlink(img);
    printf("test_ext OK\n");
}

static void test_btrfs(void) {
    char img[] = "/tmp/fsbtrXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 0x10000 + 1024);
    unsigned char sb[576] = {0};
    memcpy(sb + 64, "_BHRfS_M", 8);
    unsigned char fsid[16] = {0x90,0x55,0x7b,0xe5,0x57,0xa8,0x4f,0xf5,
                              0xbc,0x32,0xe1,0xbc,0x83,0xbe,0x6d,0x75};
    unsigned char sub[16]  = {0x94,0xec,0xd0,0xf5,0x5b,0x70,0x41,0x3e,
                              0xb7,0x1f,0xdd,0x67,0x60,0x66,0x8f,0x32};
    memcpy(sb + 32, fsid, 16);
    memcpy(sb + 267, sub, 16);
    memcpy(sb + 299, "fedora", 6);
    wr_img(img, 0x10000, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_btrfs(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "btrfs"));
    assert(fs_has(&e, "ID_FS_UUID", "90557be5-57a8-4ff5-bc32-e1bc83be6d75"));
    assert(fs_has(&e, "ID_FS_UUID_SUB", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_UUID_SUB_ENC", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_LABEL", "fedora"));
    assert(fs_absent(&e, "ID_FS_VERSION"));

    unlink(img);
    printf("test_btrfs OK\n");
}

int main(void) {
    test_helpers();
    test_ext();
    test_btrfs();
    printf("ALL blkid_fs tests passed\n");
    return 0;
}
