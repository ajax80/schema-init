#include "../static_nodes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char *mkroot(void) {
    static char t[64];
    snprintf(t, sizeof t, "/tmp/schema-sn-XXXXXX");
    char *r = mkdtemp(t);
    assert(r);
    char p[256];
    snprintf(p, sizeof p, "%s/dev", r);
    assert(mkdir(p, 0755) == 0);
    return r;
}

static void devname_char(void) {
    struct sn_node n;
    assert(sn_parse_devname_line("tun net/tun c10:200", &n) == 1);
    assert(strcmp(n.devname, "net/tun") == 0);
    assert(n.type == 'c' && n.maj == 10 && n.min == 200);
}

static void devname_block(void) {
    struct sn_node n;
    assert(sn_parse_devname_line("loop loop0 b7:0\n", &n) == 1);
    assert(strcmp(n.devname, "loop0") == 0);
    assert(n.type == 'b' && n.maj == 7 && n.min == 0);
}

static void devname_skip(void) {
    struct sn_node n;
    const char *bad[] = {
        "# Device nodes to trigger on-demand module loading.", "", "\n",
        "tun net/tun", "tun net/tun x10:200", "tun net/tun c10",
        "x ../etc/shadow c1:1", "x net/../../etc/shadow c1:1", "x /etc/shadow c1:1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        assert(sn_parse_devname_line(bad[i], &n) == 0);
}

static void create_parents_only(void) {
    char *r = mkroot();
    char f[256], p[256];
    snprintf(f, sizeof f, "%s/modules.devname", r);
    put(f, "# comment\ntun net/tun c10:200\n");
    char created[256];
    sn_create_nodes(r, f, created, sizeof created);
    struct stat st;
    snprintf(p, sizeof p, "%s/dev/net", r);
    assert(stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0755);
    snprintf(p, sizeof p, "%s/dev/net/tun", r);
    assert(stat(p, &st) != 0 || !S_ISDIR(st.st_mode));
}

static void create_missing_file(void) {
    char *r = mkroot();
    char created[64] = "x";
    assert(sn_create_nodes(r, "/nonexistent/modules.devname", created, sizeof created) == 0);
    assert(created[0] == '\0');
}

static void create_mknod(void) {
    if (geteuid() != 0) { printf("SKIP  create_mknod (not root)\n"); return; }
    char *r = mkroot();
    char f[256], p[256], created[256];
    snprintf(f, sizeof f, "%s/modules.devname", r);
    put(f, "tun net/tun c10:200\nloop loop0 b7:0\n");
    assert(sn_create_nodes(r, f, created, sizeof created) == 2);
    assert(strcmp(created, "net/tun loop0") == 0);
    struct stat st;
    snprintf(p, sizeof p, "%s/dev/net/tun", r);
    assert(stat(p, &st) == 0 && S_ISCHR(st.st_mode));
    assert(major(st.st_rdev) == 10 && minor(st.st_rdev) == 200);
    assert((st.st_mode & 07777) == 0600);
    snprintf(p, sizeof p, "%s/dev/loop0", r);
    assert(stat(p, &st) == 0 && S_ISBLK(st.st_mode));
    /* second boot pass: everything exists, nothing created, no error */
    assert(sn_create_nodes(r, f, created, sizeof created) == 0);
    assert(created[0] == '\0');
}

static void perm_parse(void) {
    struct sn_perm p;
    assert(sn_parse_perm_line("z /dev/net/tun      0666 - -     -\n", &p) == 1);
    assert(strcmp(p.path, "/dev/net/tun") == 0 && p.has_mode && p.mode == 0666);
    assert(p.user[0] == '\0' && p.group[0] == '\0');
    assert(sn_parse_perm_line("z /dev/kvm 0666 - kvm -", &p) == 1);
    assert(strcmp(p.group, "kvm") == 0 && p.user[0] == '\0');
    assert(sn_parse_perm_line("z /dev/x 06x6 - audio -", &p) == 1);
    assert(!p.has_mode && strcmp(p.group, "audio") == 0);
    assert(sn_parse_perm_line("z /dev/x - 0 5", &p) == 1);
    assert(!p.has_mode && strcmp(p.user, "0") == 0 && strcmp(p.group, "5") == 0);
}

static void perm_parse_skip(void) {
    struct sn_perm p;
    const char *bad[] = {
        "d /dev/foo 0755 - - -", "z /etc/shadow 0666 - - -",
        "z /dev/../etc/shadow 0666 - - -", "# z /dev/a 0666 - - -", "", "z",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        assert(sn_parse_perm_line(bad[i], &p) == 0);
}

static void mkpermfile(const char *r, const char *dir, const char *text) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", r, dir);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", p);
    assert(system(cmd) == 0);
    snprintf(p, sizeof p, "%s/%s/static-nodes-permissions.conf", r, dir);
    put(p, text);
}

static void perms_precedence(void) {
    char *r = mkroot();
    char out[256], want[256];
    assert(sn_find_perm_file(r, out, sizeof out) == -1);
    mkpermfile(r, "run/tmpfiles.d", "");
    assert(sn_find_perm_file(r, out, sizeof out) == -1);
    mkpermfile(r, "usr/lib/tmpfiles.d", "");
    snprintf(want, sizeof want, "%s/usr/lib/tmpfiles.d/static-nodes-permissions.conf", r);
    assert(sn_find_perm_file(r, out, sizeof out) == 0 && strcmp(out, want) == 0);
    mkpermfile(r, "etc/tmpfiles.d", "");
    snprintf(want, sizeof want, "%s/etc/tmpfiles.d/static-nodes-permissions.conf", r);
    assert(sn_find_perm_file(r, out, sizeof out) == 0 && strcmp(out, want) == 0);
}

static void perms_enoent_silent(void) {
    char *r = mkroot();
    char f[256], p[256];
    snprintf(f, sizeof f, "%s/perm.conf", r);
    put(f, "z /dev/kvm 0666 - kvm -\n");
    sn_apply_perms(r, f);
    snprintf(p, sizeof p, "%s/dev/kvm", r);
    assert(access(p, F_OK) != 0);
}

static void perms_skip_non_device(void) {
    char *r = mkroot();
    char f[256], p[256];
    snprintf(p, sizeof p, "%s/dev/fuse", r);
    put(p, "");
    chmod(p, 0600);
    snprintf(f, sizeof f, "%s/perm.conf", r);
    put(f, "z /dev/fuse 0666 - - -\n");
    sn_apply_perms(r, f);
    struct stat st;
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0600);
}

static void perms_on_nodes(void) {
    if (geteuid() != 0) { printf("SKIP  perms_on_nodes (not root)\n"); return; }
    char *r = mkroot();
    char f[256], p[256];
    snprintf(p, sizeof p, "%s/dev/tun", r);
    assert(mknod(p, S_IFCHR | 0600, makedev(10, 200)) == 0);
    snprintf(f, sizeof f, "%s/perm.conf", r);
    struct stat st;

    put(f, "z /dev/tun 0666 - - -\n");
    sn_apply_perms(r, f);
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0666 && st.st_gid == 0);

    /* perms_unknown_group_keeps_mode */
    put(f, "z /dev/tun 0660 - nosuchgroup-sn -\n");
    sn_apply_perms(r, f);
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0660 && st.st_gid == 0);

    /* numeric group; '-' mode leaves 0660 */
    put(f, "z /dev/tun - - 5 -\n");
    sn_apply_perms(r, f);
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0660 && st.st_gid == 5);

    /* named group resolves */
    put(f, "z /dev/tun - - root -\n");
    sn_apply_perms(r, f);
    assert(stat(p, &st) == 0 && st.st_gid == 0);

    /* a symlink in /dev is never followed */
    char l[256];
    snprintf(l, sizeof l, "%s/dev/link", r);
    assert(symlink(p, l) == 0);
    put(f, "z /dev/link 0600 - - -\n");
    sn_apply_perms(r, f);
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0660);
}

static void devname_file_fallback(void) {
    char *r = mkroot();
    char out[256], want[256];
    assert(sn_find_devname_file(r, "6.19.10", out, sizeof out) == -1);
    mkpermfile(r, "usr/lib/modules/6.19.10", "");
    snprintf(want, sizeof want, "%s/usr/lib/modules/6.19.10/modules.devname", r);
    put(want, "");
    assert(sn_find_devname_file(r, "6.19.10", out, sizeof out) == 0 && strcmp(out, want) == 0);
    mkpermfile(r, "lib/modules/6.19.10", "");
    snprintf(want, sizeof want, "%s/lib/modules/6.19.10/modules.devname", r);
    put(want, "");
    assert(sn_find_devname_file(r, "6.19.10", out, sizeof out) == 0 && strcmp(out, want) == 0);
}

/* a root+path too long for the buffer must be skipped, not truncated onto a
 * shorter node that happens to exist */
static void perms_truncation_skipped(void) {
    if (geteuid() != 0) { printf("SKIP  perms_truncation_skipped (not root)\n"); return; }
    char *base = mkroot();
    char r[512], p[600];
    size_t want = 511 - strlen("/dev/ne");
    snprintf(r, sizeof r, "%s/", base);
    while (strlen(r) < want) {
        size_t left = want - strlen(r);
        char comp[201];
        size_t n = left > 200 ? 200 : left;
        if (left > 200 && left - 200 < 2) n = left - 2;
        memset(comp, 'a', n); comp[n] = '\0';
        strcat(r, comp);
        if (strlen(r) < want) strcat(r, "/");
        assert(mkdir(r, 0755) == 0 || errno == EEXIST);
    }
    assert(strlen(r) == want);
    snprintf(p, sizeof p, "%s/dev", r);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof p, "%s/dev/ne", r);
    assert(mknod(p, S_IFCHR | 0600, makedev(10, 200)) == 0);
    char f[256];
    snprintf(f, sizeof f, "%s/perm.conf", base);
    put(f, "z /dev/net/tun 0666 - - -\n");
    sn_apply_perms(r, f);
    struct stat st;
    assert(stat(p, &st) == 0 && (st.st_mode & 07777) == 0600);
}

int main(void) {
    umask(077);
    perms_truncation_skipped();
    devname_file_fallback();
    perm_parse();
    perm_parse_skip();
    perms_precedence();
    perms_enoent_silent();
    perms_skip_non_device();
    perms_on_nodes();
    devname_char();
    devname_block();
    devname_skip();
    create_parents_only();
    create_missing_file();
    create_mknod();
    printf("test_static_nodes: ok\n");
    return 0;
}
