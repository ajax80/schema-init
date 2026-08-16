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
    sb[24] = 2;                                           /* s_log_block_size=2 -> 4096 */
    sb[4] = 0xE8; sb[5] = 0x03;                           /* s_blocks_count_lo = 1000 */
    wr_img(img, 1024, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_ext(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ext4"));
    assert(fs_has(&e, "ID_FS_USAGE", "filesystem"));
    assert(fs_has(&e, "ID_FS_UUID", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_UUID_ENC", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    assert(fs_has(&e, "ID_FS_VERSION", "1.0"));
    assert(fs_absent(&e, "ID_FS_LABEL"));
    assert(fs_has(&e, "ID_FS_BLOCKSIZE", "4096"));
    assert(fs_has(&e, "ID_FS_LASTBLOCK", "1000"));
    assert(fs_has(&e, "ID_FS_SIZE", "4096000"));

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
    sb[112] = 0x00; sb[113] = 0x00; sb[114] = 0x7D;       /* total_bytes = 8192000 */
    sb[144] = 0x00; sb[145] = 0x10;                       /* sectorsize = 4096 */
    wr_img(img, 0x10000, sb, sizeof sb);

    struct uevent e; e.n = 0;
    assert(fs_probe_btrfs(img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "btrfs"));
    assert(fs_has(&e, "ID_FS_UUID", "90557be5-57a8-4ff5-bc32-e1bc83be6d75"));
    assert(fs_has(&e, "ID_FS_UUID_SUB", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_UUID_SUB_ENC", "94ecd0f5-5b70-413e-b71f-dd6760668f32"));
    assert(fs_has(&e, "ID_FS_LABEL", "fedora"));
    assert(fs_absent(&e, "ID_FS_VERSION"));
    assert(fs_has(&e, "ID_FS_BLOCKSIZE", "4096"));
    assert(fs_has(&e, "ID_FS_LASTBLOCK", "2000"));
    assert(fs_has(&e, "ID_FS_SIZE", "8192000"));

    unlink(img);
    printf("test_btrfs OK\n");
}

static void test_vfat_swap(void) {
    /* vfat FAT32: serial 7c 76 73 07 -> 0773-767C; label "NO NAME    " -> none */
    char v[] = "/tmp/fsvfatXXXXXX"; int vf = mkstemp(v); assert(vf >= 0); close(vf);
    zero_img(v, 512);
    unsigned char bs[512] = {0};
    bs[11] = 0x00; bs[12] = 0x02;   /* bytes/sector = 512 */
    bs[13] = 8;                     /* sec/cluster -> cluster 4096 */
    bs[32] = 0xA0; bs[33] = 0x86; bs[34] = 0x01;                     /* total_sectors_32 = 100000 */
    /* fatsz16 (@22) = 0 -> FAT32 */
    bs[67] = 0x7c; bs[68] = 0x76; bs[69] = 0x73; bs[70] = 0x07;      /* serial */
    memcpy(bs + 71, "NO NAME    ", 11);                              /* no label */
    bs[510] = 0x55; bs[511] = 0xaa;
    wr_img(v, 0, bs, sizeof bs);

    struct uevent e; e.n = 0;
    assert(fs_probe_vfat(v, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "vfat"));
    assert(fs_has(&e, "ID_FS_UUID", "0773-767C"));
    assert(fs_has(&e, "ID_FS_VERSION", "FAT32"));
    assert(fs_absent(&e, "ID_FS_LABEL"));
    assert(fs_has(&e, "ID_FS_BLOCKSIZE", "4096"));
    assert(fs_has(&e, "ID_FS_SIZE", "51200000"));
    assert(fs_absent(&e, "ID_FS_LASTBLOCK"));
    unlink(v);

    /* labeled FAT32 */
    char v2[] = "/tmp/fsvfat2XXXXXX"; int vf2 = mkstemp(v2); assert(vf2 >= 0); close(vf2);
    zero_img(v2, 512);
    bs[13] = 8; memcpy(bs + 71, "MYSTICK    ", 11);
    wr_img(v2, 0, bs, sizeof bs);
    struct uevent e2; e2.n = 0;
    assert(fs_probe_vfat(v2, &e2) == 0);
    assert(fs_has(&e2, "ID_FS_LABEL", "MYSTICK"));
    unlink(v2);

    /* swap: version 1, uuid c2e50da1..., usage other, page 4096 */
    char s[] = "/tmp/fsswapXXXXXX"; int sf = mkstemp(s); assert(sf >= 0); close(sf);
    zero_img(s, 8192);
    unsigned char hdr[64] = {0};
    hdr[0] = 1;                     /* version @1024 = 1 */
    hdr[4] = 0xD0; hdr[5] = 0x07;   /* last_page = 2000 */
    unsigned char su[16] = {0xc2,0xe5,0x0d,0xa1,0x0f,0x93,0x4f,0x2b,
                            0x81,0x32,0x29,0xe3,0x14,0xf2,0xc8,0x27};
    memcpy(hdr + 12, su, 16);       /* uuid @1036 */
    wr_img(s, 1024, hdr, sizeof hdr);
    unsigned char mg[10]; memcpy(mg, "SWAPSPACE2", 10);
    wr_img(s, 4096 - 10, mg, 10);
    struct uevent e3; e3.n = 0;
    assert(fs_probe_swap(s, &e3) == 0);
    assert(fs_has(&e3, "ID_FS_TYPE", "swap"));
    assert(fs_has(&e3, "ID_FS_USAGE", "other"));
    assert(fs_has(&e3, "ID_FS_UUID", "c2e50da1-0f93-4f2b-8132-29e314f2c827"));
    assert(fs_has(&e3, "ID_FS_VERSION", "1"));
    assert(fs_has(&e3, "ID_FS_BLOCKSIZE", "4096"));
    assert(fs_has(&e3, "ID_FS_LASTBLOCK", "2001"));
    assert(fs_has(&e3, "ID_FS_SIZE", "8192000"));
    unlink(s);

    printf("test_vfat_swap OK\n");
}

static void test_ntfs_exfat(void) {
    /* NTFS: bps=512, spc=8 -> cluster 4096; mft_lcn=1 -> mft @4096; rec_desc=-10 -> rec 1024.
       $Volume is record 3 -> mft@4096 + 3*1024 = 7168. Serial @72 (8B) reversed. */
    char n[] = "/tmp/fsntfsXXXXXX"; int nf = mkstemp(n); assert(nf >= 0); close(nf);
    zero_img(n, 16384);
    unsigned char bs[512] = {0};
    memcpy(bs + 3, "NTFS    ", 8);
    bs[11] = 0x00; bs[12] = 0x02;   /* bytes/sector = 512 */
    bs[13] = 8;                     /* sec/cluster -> cluster 4096 */
    bs[48] = 1;                     /* mft_lcn = 1 (u64 LE) */
    bs[64] = (unsigned char)(-10);  /* clusters_per_mft_record = -10 -> 1<<10 = 1024 */
    /* serial @72: bytes -> printed reversed as 6E54847B54844833 */
    unsigned char ser[8] = {0x33,0x48,0x84,0x54,0x7b,0x84,0x54,0x6e};
    memcpy(bs + 72, ser, 8);
    bs[510] = 0x55; bs[511] = 0xaa;
    wr_img(n, 0, bs, sizeof bs);

    /* MFT record 3 @ 7168: "FILE" header, usa_off=48 usa_cnt=3, first_attr@56,
       one $VOLUME_NAME (0x60) resident attr with value "RECOVERY" (UTF-16LE). */
    unsigned char rec[1024] = {0};
    memcpy(rec, "FILE", 4);
    rec[4] = 48; rec[5] = 0;        /* usa_offset = 48 */
    rec[6] = 3;  rec[7] = 0;        /* usa_count = 3 */
    rec[20] = 56; rec[21] = 0;      /* first attribute offset = 56 */
    /* attribute @56: type 0x60, len 40, non-res=0, name_len=0, value_len, value_off */
    unsigned char *a = rec + 56;
    a[0] = 0x60;                    /* type $VOLUME_NAME */
    a[4] = 40;                      /* attr length = 40 */
    a[16] = 16; a[17] = 0;          /* value_length = 16 (8 UTF-16 chars) */
    a[20] = 24; a[21] = 0;          /* value_offset = 24 */
    const char *L = "RECOVERY";
    for (int i = 0; L[i]; i++) a[24 + i*2] = (unsigned char)L[i];
    /* terminator after the attr */
    rec[56 + 40] = 0xff; rec[56 + 41] = 0xff; rec[56 + 42] = 0xff; rec[56 + 43] = 0xff;
    /* fixup: usa[0]=signature word; sectors' last 2 bytes must equal usa[0] then get restored.
       Keep signature 0 so the fixup writes zeros (harmless for our short attr region). */
    wr_img(n, 7168, rec, sizeof rec);

    struct uevent e; e.n = 0;
    assert(fs_probe_ntfs(n, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ntfs"));
    assert(fs_has(&e, "ID_FS_UUID", "6E54847B54844833"));
    assert(fs_has(&e, "ID_FS_LABEL", "RECOVERY"));
    unlink(n);

    /* exfat: serial @100 = 0x12345678 -> "1234-5678" */
    char x[] = "/tmp/fsexfatXXXXXX"; int xf = mkstemp(x); assert(xf >= 0); close(xf);
    zero_img(x, 512);
    unsigned char xb[512] = {0};
    memcpy(xb + 3, "EXFAT   ", 8);
    xb[100] = 0x78; xb[101] = 0x56; xb[102] = 0x34; xb[103] = 0x12;   /* serial LE */
    wr_img(x, 0, xb, sizeof xb);
    struct uevent e2; e2.n = 0;
    assert(fs_probe_exfat(x, &e2) == 0);
    assert(fs_has(&e2, "ID_FS_TYPE", "exfat"));
    assert(fs_has(&e2, "ID_FS_UUID", "1234-5678"));
    unlink(x);

    printf("test_ntfs_exfat OK\n");
}

static void test_build(void) {
    /* reuse the ext4 image path: build a real ext4 sb, drive the orchestrator */
    char img[] = "/tmp/fsbuildXXXXXX"; int fd = mkstemp(img); assert(fd >= 0); close(fd);
    zero_img(img, 4096);
    unsigned char sb[264] = {0};
    sb[56] = 0x53; sb[57] = 0xef;
    unsigned char uu[16] = {0xcf,0x4f,0x2b,0x07,0xf1,0x50,0x40,0x4f,
                            0xbd,0xe1,0x4d,0x9f,0x54,0x55,0x94,0xb4};
    memcpy(sb + 104, uu, 16); sb[0x4C] = 1; sb[0x60] = 0x40;
    wr_img(img, 1024, sb, sizeof sb);

    struct uevent e;
    assert(blkid_fs_build("/sys", "/devices/x", img, &e) == 0);
    assert(fs_has(&e, "ID_FS_TYPE", "ext4"));
    assert(fs_has(&e, "ID_FS_UUID", "cf4f2b07-f150-404f-bde1-4d9f545594b4"));
    unlink(img);

    /* unformatted device -> nothing */
    char z[] = "/tmp/fszeroXXXXXX"; int zf = mkstemp(z); assert(zf >= 0); close(zf);
    zero_img(z, 65536 + 1024);
    struct uevent e2;
    assert(blkid_fs_build("/sys", "/devices/z", z, &e2) == 0);
    assert(e2.n == 0);
    unlink(z);

    printf("test_build OK\n");
}

int main(void) {
    test_helpers();
    test_ext();
    test_btrfs();
    test_vfat_swap();
    test_ntfs_exfat();
    test_build();
    printf("ALL blkid_fs tests passed\n");
    return 0;
}
