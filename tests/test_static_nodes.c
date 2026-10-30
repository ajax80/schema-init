#include "../static_nodes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

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

int main(void) {
    devname_char();
    devname_block();
    devname_skip();
    create_parents_only();
    create_missing_file();
    create_mknod();
    printf("test_static_nodes: ok\n");
    return 0;
}
