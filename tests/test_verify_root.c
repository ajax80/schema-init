#include "../verify_root.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static const char *tmpd = "/tmp";
static char devdir[PATH_MAX / 2];
static grubenv_t env[8];

static void touch(const char *p) {
    FILE *f = fopen(p, "w");
    assert(f);
    fclose(f);
}

static void mk(const char *fmt, const char *a) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, fmt, devdir, a);
    mkdir(p, 0755);
}

static void link_to(const char *dir, const char *name, const char *target) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/disk/%s/%s", devdir, dir, name);
    assert(symlink(target, p) == 0);
}

static void setup_devdir(void) {
    char p[PATH_MAX];
    snprintf(devdir, sizeof devdir, "%s/vr-dev", tmpd);
    mkdir(devdir, 0755);
    mk("%s/%s", "disk");
    mk("%s/disk/%s", "by-uuid");
    mk("%s/disk/%s", "by-partuuid");
    mk("%s/disk/%s", "by-label");
    mk("%s/disk/%s", "by-partlabel");
    mk("%s/%s", "block");
    snprintf(p, sizeof p, "%s/nvme0n1p3", devdir);
    touch(p);
    link_to("by-uuid", "1111-2222", "../../nvme0n1p3");
    link_to("by-partuuid", "3e078549-8379-4ae4-9ae3-bacc2ea36956", "../../nvme0n1p3");
    /* udev escapes a space in a label as \x20 */
    link_to("by-label", "my\\x20root", "../../nvme0n1p3");
    link_to("by-uuid", "dead-beef", "../../gone");
    link_to("by-partlabel", "root", "../../nvme0n1p3");
    link_to("by-uuid", "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9", "../../nvme0n1p3");
    link_to("by-uuid", "ABCD-1234", "../../nvme0n1p3");
    snprintf(p, sizeof p, "%s/block/259:3", devdir);
    assert(symlink("../nvme0n1p3", p) == 0);
    setenv("SCHEMA_VERIFY_DEVDIR", devdir, 1);
}

static void test_dev_resolve(void) {
    char out[PATH_MAX], node[PATH_MAX], want[PATH_MAX];
    snprintf(node, sizeof node, "%s/nvme0n1p3", devdir);
    assert(realpath(node, want));

    assert(dev_resolve("UUID=1111-2222", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    /* the kernel compares PARTUUID case-insensitively; udev's links are lower case */
    assert(dev_resolve("PARTUUID=3E078549-8379-4AE4-9AE3-BACC2EA36956", devdir, out, sizeof out) == 0 &&
           !strcmp(out, want));
    assert(dev_resolve("LABEL=my root", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    assert(dev_resolve("/dev/nvme0n1p3", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    /* NULL devdir takes the env override */
    assert(dev_resolve("UUID=1111-2222", NULL, out, sizeof out) == 0 && !strcmp(out, want));

    /* udev lower-cases ext4/btrfs/xfs UUIDs and dracut does too before the lookup; a vfat
     * UUID's link stays upper case, so the value as written is tried first */
    assert(dev_resolve("UUID=0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    assert(dev_resolve("UUID=ABCD-1234", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    assert(dev_resolve("UUID=abcd-1234", devdir, out, sizeof out) == -1);
    assert(dev_resolve("UUID=dead-beef", devdir, out, sizeof out) == -1);
    assert(dev_resolve("UUID=nope", devdir, out, sizeof out) == -1);
    assert(dev_resolve("UUID=", devdir, out, sizeof out) == -1);
    assert(dev_resolve("/dev/", devdir, out, sizeof out) == -1);
    assert(dev_resolve("UUID=../../nvme0n1p3", devdir, out, sizeof out) == -1);
    assert(dev_resolve("/dev/sda9", devdir, out, sizeof out) == -1);
    /* maj:min through /dev/block, as the kernel takes it */
    assert(dev_resolve("259:3", devdir, out, sizeof out) == 0 && !strcmp(out, want));
    assert(dev_resolve("259:4", devdir, out, sizeof out) == -1);
    assert(dev_resolve("259:3/../x", devdir, out, sizeof out) == -1);
}

static void test_path_map(void) {
    char out[PATH_MAX];
    /* blakbox: GRUB's root is the btrfs top level, mounted at mnt */
    assert(bls_path_map("/tmp/mnt", "/root/boot/vmlinuz-x", out, sizeof out) == 0);
    assert(!strcmp(out, "/tmp/mnt/root/boot/vmlinuz-x"));
    assert(bls_path_map("/tmp/mnt/", "/schema-snap-1/boot/vmlinuz-x", out, sizeof out) == 0);
    assert(!strcmp(out, "/tmp/mnt/schema-snap-1/boot/vmlinuz-x"));
    /* separate /boot: the entry's paths are relative to that partition */
    assert(bls_path_map("/boot", "/vmlinuz-x", out, sizeof out) == 0 && !strcmp(out, "/boot/vmlinuz-x"));
    assert(bls_path_map("/", "/vmlinuz-x", out, sizeof out) == 0 && !strcmp(out, "/vmlinuz-x"));

    assert(bls_path_map("/boot", "vmlinuz-x", out, sizeof out) == -1);
    assert(bls_path_map("/boot", "/../etc/shadow", out, sizeof out) == -1);
    assert(bls_path_map("/boot", "/a/..", out, sizeof out) == -1);
    assert(bls_path_map("/boot", "/a/..b", out, sizeof out) == 0);
    char small[8];
    assert(bls_path_map("/boot", "/vmlinuz-x", small, sizeof small) == -1);
}

/* GRUB resolves a symlink in the boot tree against its own root, never the host's */
static void test_in_root(void) {
    char root[PATH_MAX / 2], p[PATH_MAX];
    struct stat st;
    snprintf(root, sizeof root, "%s/vr-top", tmpd);
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/boot", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/boot/vmlinuz-x", root);
    touch(p);
    snprintf(p, sizeof p, "%s/boot/rel", root);
    assert(symlink("vmlinuz-x", p) == 0);
    /* /etc/passwd exists on the host, not in this tree */
    snprintf(p, sizeof p, "%s/boot/abs", root);
    assert(symlink("/etc/passwd", p) == 0);
    snprintf(p, sizeof p, "%s/snap", root);
    assert(symlink("/boot", p) == 0);

    assert(stat_in_root(root, "/boot/vmlinuz-x", 0, &st) == 0 && S_ISREG(st.st_mode));
    assert(stat_in_root(root, "/boot/rel", 0, &st) == 0 && S_ISREG(st.st_mode));
    assert(stat_in_root(root, "/boot/abs", 0, &st) == -1);
    assert(stat_in_root(root, "/boot/nope", 0, &st) == -1);
    assert(stat_in_root(root, "/boot/vmlinuz-x/x", 0, &st) == -1);
    assert(stat_in_root(root, "/../boot/vmlinuz-x", 0, &st) == -2);
    /* the subvolume check refuses any symlink */
    assert(stat_in_root(root, "/boot", 1, &st) == 0 && S_ISDIR(st.st_mode));
    assert(stat_in_root(root, "/snap", 1, &st) == -2);
    assert(stat_in_root(root, "/snap", 0, &st) == 0 && S_ISDIR(st.st_mode));
}

static void test_boot_separate(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/vr-mountinfo-blakbox", tmpd);
    FILE *f = fopen(p, "w");
    fprintf(f, "26 1 0:25 /root / rw,relatime shared:1 - btrfs /dev/nvme0n1p3 rw,subvol=/root\n"
               "60 26 259:1 / /boot/efi rw,relatime shared:31 - vfat /dev/nvme0n1p1 rw\n"
               "61 26 0:25 /home /home rw,relatime shared:32 - btrfs /dev/nvme0n1p3 rw\n");
    fclose(f);
    assert(boot_is_separate(p) == 0);

    snprintf(p, sizeof p, "%s/vr-mountinfo-sepboot", tmpd);
    f = fopen(p, "w");
    fprintf(f, "26 1 8:3 / / rw,relatime shared:1 - ext4 /dev/sda3 rw\n"
               "59 26 8:2 / /boot rw,relatime shared:30 - ext4 /dev/sda2 rw\n"
               "60 59 8:1 / /boot/efi rw,relatime shared:31 - vfat /dev/sda1 rw\n");
    fclose(f);
    assert(boot_is_separate(p) == 1);
    assert(boot_is_separate("/nonexistent/mountinfo") == 0);
}

static void test_expand(void) {
    char out[4096], miss[256];
    snprintf(env[0][0], 256, "saved_entry");
    snprintf(env[0][1], 256, "x");
    snprintf(env[1][0], 256, "kernelopts");
    snprintf(env[1][1], 256, "root=UUID=1111-2222 ro rootflags=subvol=root");
    snprintf(env[2][0], 256, "tuned_params");
    env[2][1][0] = '\0';

    assert(opts_expand("$kernelopts quiet", env, 3, out, sizeof out, miss, sizeof miss) == 0);
    assert(!strcmp(out, "root=UUID=1111-2222 ro rootflags=subvol=root quiet"));
    assert(opts_expand("${kernelopts} $tuned_params", env, 3, out, sizeof out, miss, sizeof miss) == 0);
    assert(!strcmp(out, "root=UUID=1111-2222 ro rootflags=subvol=root "));
    assert(opts_expand("root=/dev/sda1 ro", env, 3, out, sizeof out, miss, sizeof miss) == 0);
    assert(!strcmp(out, "root=/dev/sda1 ro"));
    assert(opts_expand("$kernelopts $tuned_initrd", env, 3, out, sizeof out, miss, sizeof miss) == -1);
    assert(!strcmp(miss, "tuned_initrd"));
    assert(opts_expand("${nope", env, 3, out, sizeof out, miss, sizeof miss) == -1);
    /* grubenv_read keeps a value too long to read as cut: never expanded short */
    snprintf(env[3][0], sizeof env[3][0], "longopts");
    snprintf(env[3][1], sizeof env[3][1], GRUBENV_CUT);
    assert(opts_expand("$longopts quiet", env, 4, out, sizeof out, miss, sizeof miss) == -3);
    assert(!strcmp(miss, "longopts"));
}

static bls_entry_t entry(const char *opts) {
    bls_entry_t e;
    memset(&e, 0, sizeof e);
    snprintf(e.id, sizeof e.id, "t");
    snprintf(e.linux_path, sizeof e.linux_path, "/root/boot/vmlinuz-x");
    e.n_initrd = 1;
    snprintf(e.initrd[0], sizeof e.initrd[0], "/root/boot/initramfs-x.img");
    snprintf(e.options, sizeof e.options, "%s", opts);
    return e;
}

static int has(char (*v)[256], int n, const char *s) {
    for (int i = 0; i < n; i++)
        if (strstr(v[i], s)) return 1;
    return 0;
}

static void test_precheck(void) {
    root_report_t r;
    char opts[4096];
    bls_entry_t e = entry("root=UUID=1111-2222 ro rootflags=subvol=root init=/sbin/schema-init");

    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(!r.fail && !r.nunres && !r.unresolved);

    /* $kernelopts comes from grubenv */
    e = entry("$kernelopts init=/sbin/schema-init");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0 && !r.fail && !r.nunres);
    assert(!strcmp(opts, "root=UUID=1111-2222 ro rootflags=subvol=root init=/sbin/schema-init"));

    /* a variable grubenv doesn't hold is unknown, never a FAIL */
    e = entry("$kernelopts $tuned_params2");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == -1);
    assert(!r.fail && r.nunres == 1 && !strcmp(r.unres[0], "options use $tuned_params2, not in grubenv or grub.cfg"));

    /* set in grubenv and grub.cfg to different values: which GRUB ends with is not known */
    static grubenv_t both[1];
    snprintf(both[0][0], 256, "both");
    snprintf(both[0][1], 256, GRUBENV_BOTH);
    e = entry("root=UUID=1111-2222 $both");
    assert(entry_precheck(&e, both, 1, opts, sizeof opts, &r) == -1);
    assert(!r.fail && r.nunres == 1 &&
           !strcmp(r.unres[0], "$both is set in both grubenv and grub.cfg with different values"));
    e = entry("$kernelopts $tuned_params2");
    entry_precheck(&e, env, 3, opts, sizeof opts, &r);
    assert(r.unresolved == r.unres[0]);
    e = entry("$longopts");
    assert(entry_precheck(&e, env, 4, opts, sizeof opts, &r) == -1);
    assert(!r.fail && has(r.unres, r.nunres, "$longopts is too long"));
    e = entry("$kernelopts");
    assert(entry_precheck(&e, env, 1, opts, sizeof opts, &r) == -1);
    assert(!r.fail && has(r.unres, r.nunres, "$kernelopts"));

    e = entry("root=UUID=1111-2222 ro");
    e.extra_options = 1;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(!r.fail && has(r.unres, r.nunres, "several options lines"));
    e.extra_options = 0;
    e.extra_linux = 1;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(!r.fail && has(r.unres, r.nunres, "several linux lines"));

    /* initramfs consumes SCHEMA_SVC_DIR=; init= is passed on by switch_root */
    e = entry("root=UUID=1111-2222 init=/sbin/schema-init SCHEMA_SVC_DIR=/etc/schema-init/known-good");
    e.n_initrd = 1;
    snprintf(e.initrd[0], sizeof e.initrd[0], "/root/boot/initramfs-x.img");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(!r.fail && r.nunres == 1 && !strcmp(r.unres[0], "SCHEMA_SVC_DIR with an initramfs: not known to reach PID 1"));
    e = entry("root=UUID=1111-2222 init=/sbin/schema-init");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0 && !r.fail && !r.nunres);
    e = entry("root=PARTUUID=3e078549-8379-4ae4-9ae3-bacc2ea36956 SCHEMA_SVC_DIR=/etc/schema-init/known-good");
    e.n_initrd = 0;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0 && !r.fail && !r.nunres);

    /* no initramfs: the kernel resolves root= alone, which UUID=/LABEL=/dev/mapper need
     * udev or device-mapper for (blakbox 09-16: panic) */
    bls_entry_t f;
    assert(bls_parse("tests/fixtures/verify/boot/noinitrd-uuid.conf", &f) == 0 && !f.n_initrd);
    assert(entry_precheck(&f, env, 3, opts, sizeof opts, &r) == 0);
    assert(r.fail && has(r.reasons, r.nreasons, "root=UUID=1111-2222 with no initrd"));
    static const char *const nokernel[] = { "root=LABEL=my\\x20root", "root=/dev/mapper/luks-1", "root=/dev/disk/by-uuid/1111-2222" };
    for (size_t i = 0; i < sizeof nokernel / sizeof *nokernel; i++) {
        e = entry(nokernel[i]);
        e.n_initrd = 0;
        entry_precheck(&e, env, 3, opts, sizeof opts, &r);
        assert(r.fail && has(r.reasons, r.nreasons, "with no initrd"));
        e.n_initrd = 1;
        entry_precheck(&e, env, 3, opts, sizeof opts, &r);
        assert(!has(r.reasons, r.nreasons, "with no initrd"));
    }
    static const char *const kernel[] = { "root=PARTUUID=3e078549-8379-4ae4-9ae3-bacc2ea36956", "root=/dev/nvme0n1p3",
                                          "root=259:3", "root=PARTLABEL=root" };
    for (size_t i = 0; i < sizeof kernel / sizeof *kernel; i++) {
        e = entry(kernel[i]);
        e.n_initrd = 0;
        assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0 && !r.fail && !r.nunres);
    }
    /* initrd only $tuned_initrd: whether an initramfs loads is not known */
    e = entry("root=UUID=1111-2222");
    e.n_initrd = 0;
    e.initrd_vars = 1;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(!r.fail && has(r.unres, r.nunres, "initrd is only GRUB variables"));

    e = entry("ro quiet");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == -1);
    assert(r.fail && has(r.reasons, r.nreasons, "no root="));
    e = entry("root=UUID=1111-2222 root=UUID=1111-2223");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == -1 && r.fail);

    e = entry("root=UUID=nope ro");
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == -1);
    assert(r.fail && has(r.reasons, r.nreasons, "root=UUID=nope"));

    e = entry("root=UUID=1111-2222 ro");
    e.blank_before_last = 1;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0);
    assert(r.fail && has(r.reasons, r.nreasons, "blank line"));
    e.blank_before_last = 0;
    e.parse_err = 1;
    assert(entry_precheck(&e, env, 3, opts, sizeof opts, &r) == 0 && r.fail);
}

static char *dup_s(const char *s) {
    char *d = malloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

/* what the child sends back; the mount itself can't run unprivileged */
static void test_report_fill(void) {
    root_report_t r;
    memset(&r, 0, sizeof r);
    r.check_rc = -1;
    char ctl[] = "I /usr/bin/schema-init\nS\nC 1\n";
    char *out = dup_s("schema-verify-baseline v1\nbinary_version=x\nerror: a.svc: bad\n");
    root_report_fill(ctl, strlen(ctl), out, strlen(out), &r);
    assert(!r.fail && !r.nunres && r.is_schema_init && !strcmp(r.init_path, "/usr/bin/schema-init"));
    assert(r.check_rc == 1 && r.check_out == out && r.check_len == strlen(out));
    free(r.check_out);

    /* an init older than --check: rc 1 and no report, which must not read as a clean load */
    memset(&r, 0, sizeof r);
    r.check_rc = -1;
    char old[] = "I /usr/bin/schema-init\nS\nC 1\nE schema-init: unknown option '--check'\n";
    root_report_fill(old, strlen(old), NULL, 0, &r);
    assert(!r.fail && !r.nunres && r.predates_check && r.check_rc == -1 && !r.check_out);
    /* no report for any other reason stays unresolved */
    memset(&r, 0, sizeof r);
    r.check_rc = -1;
    char none[] = "I /usr/bin/schema-init\nS\nC 0\n";
    root_report_fill(none, strlen(none), NULL, 0, &r);
    assert(!r.predates_check && r.nunres == 1 && !strcmp(r.unres[0], "/usr/bin/schema-init --check gave no report"));
    memset(&r, 0, sizeof r);
    r.check_rc = -1;
    char notsi[] = "I /usr/bin/other\nC 1\nE schema-init: unknown option '--check'\n";
    root_report_fill(notsi, strlen(notsi), NULL, 0, &r);
    assert(!r.predates_check && r.nunres == 1);

    memset(&r, 0, sizeof r);
    r.check_rc = -1;
    char bad[] = "F kernel /root/boot/vmlinuz-x is missing\nU cannot chroot into the target: x\nI /sbin/init\n";
    root_report_fill(bad, strlen(bad), NULL, 0, &r);
    assert(r.fail && r.nreasons == 1 && !strcmp(r.reasons[0], "kernel /root/boot/vmlinuz-x is missing"));
    assert(r.nunres == 1 && r.unresolved == r.unres[0] && !r.is_schema_init && r.check_rc == -1);
    assert(r.bootok_exec == 0);

    /* schema-bootok's exec= as the child found it inside the target */
    memset(&r, 0, sizeof r);
    char bok[] = "I /usr/bin/schema-init\nS\nB 1\n";
    root_report_fill(bok, strlen(bok), NULL, 0, &r);
    assert(r.bootok_exec == 1);
    memset(&r, 0, sizeof r);
    char bbad[] = "B 2\n";
    root_report_fill(bbad, strlen(bbad), NULL, 0, &r);
    assert(r.bootok_exec == 2 && !r.fail);
}

static void mkp(const char *root, const char *sub, int dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s", root, sub);
    if (dir) assert(mkdir(p, 0755) == 0);
    else touch(p);
}

static void lnp(const char *root, const char *sub, const char *target) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s", root, sub);
    assert(symlink(target, p) == 0);
}

/* run one check with its records into a temp file, then read them back as the parent does */
#define RECORDS(r, call)                                                        \
    do {                                                                        \
        FILE *t_ = tmpfile();                                                   \
        int ctl = fileno(t_);                                                   \
        rc = call;                                                              \
        static char b_[8192];                                                   \
        size_t l_ = (size_t)pread(ctl, b_, sizeof b_ - 1, 0);                   \
        memset(&r, 0, sizeof r);                                                \
        r.check_rc = -1;                                                        \
        root_report_fill(b_, l_, NULL, 0, &r);                                  \
        fclose(t_);                                                             \
    } while (0)

/* what root_inspect checks once it has mounted: run against plain directory trees */
static void test_tree_checks(void) {
    char top[PATH_MAX / 2], tgt[PATH_MAX / 2], real[PATH_MAX];
    root_report_t r;
    int rc;
    snprintf(top, sizeof top, "%s/vr-tree-top", tmpd);
    snprintf(tgt, sizeof tgt, "%s/vr-tree-tgt", tmpd);
    mkp(top, "", 1);
    mkp(top, "/root", 1);
    mkp(top, "/root/boot", 1);
    mkp(top, "/root/boot/vmlinuz-x", 0);
    mkp(top, "/root/boot/initramfs-x.img", 0);
    lnp(top, "/snap-link", "/root");

    /* subvolume: absent FAIL, a symlink UNRESOLVED, a plain directory is not a subvolume */
    RECORDS(r, subvol_check(ctl, top, "gone,compress=zstd", "UUID=1"));
    assert(rc && r.fail && has(r.reasons, r.nreasons, "subvolume gone does not exist on UUID=1"));
    RECORDS(r, subvol_check(ctl, top, "/snap-link", "UUID=1"));
    assert(rc && !r.fail && has(r.unres, r.nunres, "snap-link has a symlink"));
    struct stat st;
    snprintf(real, sizeof real, "%s/root", top);
    assert(stat(real, &st) == 0);
    RECORDS(r, subvol_check(ctl, top, "root", "UUID=1"));
    if (st.st_ino == 256) assert(!rc && !r.fail);
    else assert(rc && r.fail && has(r.reasons, r.nreasons, "root on UUID=1 is a directory, not a subvolume"));

    /* kernel and every initrd, relative to GRUB's root */
    bls_entry_t e = entry("root=UUID=1111-2222");
    RECORDS(r, boot_files_check(ctl, top, &e));
    assert(!rc && !r.fail && !r.nunres);
    snprintf(e.linux_path, sizeof e.linux_path, "/root/boot/vmlinuz-gone");
    snprintf(e.initrd[e.n_initrd++], sizeof e.initrd[0], "/root/boot/initramfs-gone.img");
    RECORDS(r, boot_files_check(ctl, top, &e));
    assert(rc && r.nreasons == 2 && has(r.reasons, 2, "kernel /root/boot/vmlinuz-gone is missing") &&
           has(r.reasons, 2, "initrd /root/boot/initramfs-gone.img is missing"));
    e = entry("root=UUID=1111-2222");
    snprintf(e.linux_path, sizeof e.linux_path, "/root/../../etc/vmlinuz");
    RECORDS(r, boot_files_check(ctl, top, &e));
    assert(rc && !r.fail && has(r.unres, r.nunres, "kernel path /root/../../etc/vmlinuz could not be resolved"));

    /* init= resolved inside the target, before host /dev and /proc are bound in */
    mkp(tgt, "", 1);
    mkp(tgt, "/dev", 1);
    mkp(tgt, "/proc", 1);
    mkp(tgt, "/tmp", 1);
    mkp(tgt, "/usr", 1);
    mkp(tgt, "/usr/bin", 1);
    mkp(tgt, "/usr/bin/schema-init", 0);
    snprintf(real, sizeof real, "%s/usr/bin/schema-init", tgt);
    chmod(real, 0755);
    mkp(tgt, "/usr/bin/noexec", 0);
    lnp(tgt, "/sbin", "usr/bin");
    lnp(tgt, "/usr/bin/self", "/proc/self/exe");
    mkp(tgt, "/dev/init", 0);
    snprintf(real, sizeof real, "%s/dev/init", tgt);
    chmod(real, 0755);
    RECORDS(r, init_resolve(ctl, tgt, "/sbin/schema-init", real, sizeof real));
    assert(!rc && !r.fail && !r.nunres && !strcmp(real, "/usr/bin/schema-init"));
    RECORDS(r, init_resolve(ctl, tgt, "/sbin/self", real, sizeof real));
    assert(rc && r.fail && has(r.reasons, r.nreasons, "init=/sbin/self does not exist in the target"));
    RECORDS(r, init_resolve(ctl, tgt, "/proc/self/exe", real, sizeof real));
    assert(rc && r.fail);
    RECORDS(r, init_resolve(ctl, tgt, "/sbin/noexec", real, sizeof real));
    assert(rc && r.fail && has(r.reasons, r.nreasons, "not executable"));
    RECORDS(r, init_resolve(ctl, tgt, "/dev/init", real, sizeof real));
    assert(rc && !r.fail && has(r.unres, r.nunres, "init=/dev/init resolves into /dev"));
    RECORDS(r, init_resolve(ctl, tgt, "/sbin/gone", real, sizeof real));
    assert(rc && r.fail && has(r.reasons, r.nreasons, "does not exist"));

    /* /dev, /proc and /tmp must be the target's own directories before anything is mounted on them */
    RECORDS(r, target_dirs_check(ctl, tgt));
    assert(!rc && !r.fail && !r.nunres);
    snprintf(real, sizeof real, "%s/tmp", tgt);
    rmdir(real);
    lnp(tgt, "/tmp", "/var/tmp");
    RECORDS(r, target_dirs_check(ctl, tgt));
    assert(rc && !r.fail && has(r.unres, r.nunres, "/tmp in the target is not a directory"));
    unlink(real);
    RECORDS(r, target_dirs_check(ctl, tgt));
    assert(rc && has(r.unres, r.nunres, "/tmp in the target is not a directory"));
}

/* a non-btrfs root already mounted read-write: found by its device number with mount root / */
static void test_mountinfo_find(void) {
    char p[PATH_MAX], mp[PATH_MAX];
    snprintf(p, sizeof p, "%s/vr-mountinfo-ext4", tmpd);
    FILE *f = fopen(p, "w");
    fprintf(f, "26 1 8:3 / / rw,relatime shared:1 - ext4 /dev/sda3 rw\n"
               "40 26 8:5 /sub /mnt/sub rw - ext4 /dev/sda5 rw\n"
               "41 26 8:5 / /mnt/my\\040disk rw - ext4 /dev/sda5 rw\n"
               "42 26 0:25 / /home rw - btrfs /dev/nvme0n1p3 rw\n");
    fclose(f);
    assert(mountinfo_find(p, makedev(8, 3), mp, sizeof mp) == 0 && !strcmp(mp, "/"));
    /* a bind of a subdirectory is not the whole filesystem */
    assert(mountinfo_find(p, makedev(8, 5), mp, sizeof mp) == 0 && !strcmp(mp, "/mnt/my disk"));
    assert(mountinfo_find(p, makedev(8, 9), mp, sizeof mp) == -1);
    assert(mountinfo_find("/nonexistent", makedev(8, 3), mp, sizeof mp) == -1);
}

static void test_needs_root(void) {
    if (geteuid() == 0) {
        printf("verify_root: running as root, needs-root case skipped\n");
        return;
    }
    root_report_t r;
    bls_entry_t e = entry("root=UUID=1111-2222 ro rootflags=subvol=root init=/sbin/schema-init");
    assert(root_inspect(&e, env, 3, NULL, &r) == 3);
    assert(!r.fail && r.unresolved && !strcmp(r.unresolved, "needs root"));
    assert(r.check_rc == -1 && !r.check_out);
    /* the checks that need no mount still run unprivileged */
    e = entry("ro quiet");
    assert(root_inspect(&e, env, 3, NULL, &r) == 1);
    assert(r.fail && !r.unresolved);
}

static void test_rootflags_data(void) {
    char d[256];
    unsigned long ms = 0;
    /* Eli 11-22: dracut hands rootflags= to mount(8), which turns noatime into
     * MS_NOATIME; as raw mount(2) data btrfs refuses it with EINVAL */
    assert(rootflags_data("subvol=root,noatime", 1, &ms, d, sizeof d) == 0 && !strcmp(d, "subvol=root") && ms == MS_NOATIME);
    ms = 0;
    assert(rootflags_data("rw,noatime,compress=zstd:1,subvol=root,relatime,nodev,x-foo=1,defaults,user=bob,rprivate,comment=a",
                          1, &ms, d, sizeof d) == 0 && !strcmp(d, "compress=zstd:1,subvol=root") &&
           ms == (MS_NOATIME | MS_RELATIME | MS_NODEV));
    /* noexec must reach the mount, or an init the boot cannot exec passes */
    ms = 0;
    assert(rootflags_data("subvol=root,noexec", 1, &ms, d, sizeof d) == 0 && !strcmp(d, "subvol=root") && ms == MS_NOEXEC);
    ms = 0;
    assert(rootflags_data("bind,remount", 1, &ms, d, sizeof d) == 0 && !strcmp(d, "") && ms == 0);
    /* a key that only starts like one of mount(8)'s stays data */
    assert(rootflags_data("users_quota=1,user_xattr", 1, &ms, d, sizeof d) == 0 && !strcmp(d, "users_quota=1,user_xattr"));
    /* no initramfs: the kernel passes rootflags= to mount(2) untouched */
    ms = 0;
    assert(rootflags_data("subvol=root,noatime", 0, &ms, d, sizeof d) == 0 && !strcmp(d, "subvol=root,noatime") && ms == 0);
    assert(rootflags_data(NULL, 1, &ms, d, sizeof d) == 0 && !strcmp(d, ""));
    assert(rootflags_data("subvol=root,noatime", 1, &ms, d, 5) == -1);
}


/* a crafted image: only the superblock bytes needs_replay reads */
static void img(const char *path, long off, const void *b, size_t n, long off2, const void *b2, size_t n2) {
    FILE *f = fopen(path, "w");
    assert(f);
    fseek(f, off, SEEK_SET);
    fwrite(b, 1, n, f);
    if (b2) {
        fseek(f, off2, SEEK_SET);
        fwrite(b2, 1, n2, f);
    }
    fseek(f, 0x11000, SEEK_SET);
    fputc(0, f);
    fclose(f);
}

static void test_needs_replay(void) {
    char p[PATH_MAX], sys[PATH_MAX], d[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/vr-img", tmpd);
    snprintf(sys, sizeof sys, "%s/vr-sysbtrfs", tmpd);
    mkdir(sys, 0755);
    /* ext: magic 0xEF53 at sb+0x38, incompat at sb+0x60, RECOVER = 0x4 */
    unsigned char mag[2] = { 0x53, 0xef }, clean[4] = { 0xc2, 0x02, 0, 0 }, dirty[4] = { 0xc6, 0x22, 0, 0 };
    img(p, 1024 + 0x38, mag, 2, 1024 + 0x60, clean, 4);
    assert(needs_replay(p, sys) == 0);
    img(p, 1024 + 0x38, mag, 2, 1024 + 0x60, dirty, 4);
    assert(needs_replay(p, sys) == 1);
    /* btrfs: fsid at sb+0x20, magic at sb+0x40, log_root at sb+0x60 */
    unsigned char sb[0x68] = { 0 };
    for (int i = 0; i < 16; i++) sb[0x20 + i] = (unsigned char)(0x10 * i + i);
    memcpy(sb + 0x40, "_BHRfS_M", 8);
    img(p, 0x10000, sb, sizeof sb, 0, NULL, 0);
    assert(needs_replay(p, sys) == 0);
    /* a stale dirty ext superblock left in a btrfs device's first 64K is not its state */
    img(p, 0x10000, sb, sizeof sb, 1024 + 0x38, mag, 2);
    {
        FILE *f = fopen(p, "r+");
        fseek(f, 1024 + 0x60, SEEK_SET);
        fwrite(dirty, 1, 4, f);
        fclose(f);
    }
    assert(needs_replay(p, sys) == 0);
    sb[0x62] = 0xd4;
    img(p, 0x10000, sb, sizeof sb, 0, NULL, 0);
    assert(needs_replay(p, sys) == 1);
    /* mounted somewhere: the kernel replayed the log when it first mounted it */
    snprintf(d, sizeof d, "%s/00112233-4455-6677-8899-aabbccddeeff", sys);
    mkdir(d, 0755);
    assert(needs_replay(p, sys) == 0);
    rmdir(d);
    unsigned char none[8] = { 0 };
    img(p, 0, none, 8, 0, NULL, 0);
    assert(needs_replay(p, sys) == 0);
    unlink(p);
    assert(needs_replay(p, sys) == 0);
    rmdir(sys);
}

/* root + loop devices from tests/verify_nowrite_live.sh; it checks the image bytes, this the report */
static int unres_has(const root_report_t *r, const char *s) {
    for (int i = 0; i < r->nunres; i++)
        if (strstr(r->unres[i], s)) return 1;
    return 0;
}

static void live_case(const char *var, const char *opts_fmt, const char *want, const char *never) {
    const char *dev = getenv(var);
    char opts[512];
    root_report_t r;
    if (!dev) return;
    snprintf(opts, sizeof opts, opts_fmt, dev);
    bls_entry_t e = entry(opts);
    root_inspect(&e, env, 3, NULL, &r);
    for (int i = 0; i < r.nunres; i++) printf("  %s: U %s\n", var, r.unres[i]);
    if (want && !unres_has(&r, want)) { printf("FAIL live %s: no \"%s\"\n", var, want); exit(1); }
    if (never && unres_has(&r, never)) { printf("FAIL live %s: \"%s\"\n", var, never); exit(1); }
    printf("ok live %s\n", var);
}

static void test_live_nowrite(void) {
    if (geteuid() != 0 || !getenv("VL_LIVE")) return;
    unsetenv("SCHEMA_VERIFY_DEVDIR");
    live_case("VL_BTRFS_DIRTY", "root=%s ro", "needs replay", NULL);
    live_case("VL_EXT4_DIRTY", "root=%s ro", "needs replay", NULL);
    live_case("VL_XFS_DIRTY", "root=%s ro", "cannot mount", NULL);
    live_case("VL_BTRFS_CLEAN", "root=%s ro", NULL, "cannot mount");
    live_case("VL_EXT4_CLEAN", "root=%s ro", NULL, "cannot mount");
    live_case("VL_EXT4_MOUNTED", "root=%s ro rootflags=data=journal", "mounted already", NULL);
    live_case("VL_EXT4_MOUNTED", "root=%s ro rootflags=noatime,nodev", NULL, "mounted already");
}

int main(int argc, char **argv) {
    if (argc > 1) tmpd = argv[1];
    setup_devdir();
    test_dev_resolve();
    test_path_map();
    test_in_root();
    test_boot_separate();
    test_expand();
    test_precheck();
    test_report_fill();
    test_tree_checks();
    test_mountinfo_find();
    test_needs_root();
    test_needs_replay();
    test_live_nowrite();
    test_rootflags_data();
    printf("verify_root: ok\n");
    return 0;
}
