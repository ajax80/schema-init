#include "../verify_root.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    assert(dev_resolve("UUID=dead-beef", devdir, out, sizeof out) == -1);
    assert(dev_resolve("UUID=nope", devdir, out, sizeof out) == -1);
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
    assert(!r.fail && r.nunres == 1 && !strcmp(r.unres[0], "options use $tuned_params2, not in grubenv"));
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
    assert(!r.fail && r.nunres == 1 && r.check_rc == -1 && !r.check_out);
    assert(!strcmp(r.unres[0], "/usr/bin/schema-init --check gave no report: schema-init: unknown option '--check'"));

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
    test_needs_root();
    printf("verify_root: ok\n");
    return 0;
}
