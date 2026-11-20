#include "../verify_diff.h"
#include "../svc_serial.h"
#include "../sha256.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HDR "schema-verify-baseline v1\nbinary_version=0.4.20\ncreated=1763500000\n" \
            "cmdline.root=UUID=abc\nhardening_default=on\nsvc_dir=/etc/schema-init/services\n"
#define NHDR "schema-verify-baseline v1\nbinary_version=0.4.21\n" \
             "cmdline.root=UUID=abc\nhardening_default=on\nsvc_dir=/etc/schema-init/services\n"

static char tmp[4096];
static finding_t f[64];
static int nf;
static char hash[65];

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "r");
    char *b = calloc(1, 65536);
    assert(fp && b);
    fread(b, 1, 65535, fp);
    fclose(fp);
    return b;
}

static int diff(const char *base, const char *next) {
    memset(f, 0, sizeof f);
    nf = diff_baseline(base, next, f, 64, hash);
    return nf;
}

static int count(fkind_t k) {
    int c = 0;
    for (int i = 0; i < nf && i < 64; i++) c += f[i].kind == k;
    return c;
}

static const char *find(fkind_t k, const char *sub) {
    for (int i = 0; i < nf && i < 64; i++)
        if (f[i].kind == k && strstr(f[i].text, sub)) return f[i].text;
    return NULL;
}

static int is_empty_hash(const char *h) {
    char e[65];
    sha256_hex("", 0, e);
    return !strcmp(h, e);
}

static void test_1118(void) {
    char *b = slurp("tests/fixtures/verify/diff/1118-base.txt"), *n = slurp("tests/fixtures/verify/diff/1118-next.txt");
    assert(diff(b, n) == 1);
    assert(count(F_CHANGE) == 1);
    assert(strstr(f[0].text, "syscall_deny  unset → default (default)   63 services: "));
    assert(strstr(f[0].text, " …"));
    assert(!is_empty_hash(hash));
    free(b); free(n);
    printf("ok test_1118\n");
}

static void test_argv_change(void) {
    diff(HDR "mount-home.argv=/home\nmount-home.exec=/usr/bin/mount\n",
         NHDR "mount-home.argv=/tmp\nmount-home.exec=/usr/bin/mount\n");
    assert(nf == 1 && f[0].kind == F_CHANGE);
    assert(!strcmp(f[0].text, "argv  /home → /tmp   1 service: mount-home"));
    printf("ok test_argv_change\n");
}

static void test_source_only(void) {
    diff(HDR "a.protect_home=0\tsrc=default\n", NHDR "a.protect_home=0\tsrc=explicit\n");
    assert(nf == 1 && f[0].kind == F_NOTE);
    assert(strstr(f[0].text, "protect_home") && strstr(f[0].text, "default → explicit"));
    assert(is_empty_hash(hash));
    printf("ok test_source_only\n");
}

static void test_removed(void) {
    diff(HDR "a.exec=/a\nb.exec=/b\n", NHDR "a.exec=/a\n");
    assert(nf == 1 && f[0].kind == F_CHANGE && strstr(f[0].text, "b") && strstr(f[0].text, "removed"));
    assert(!is_empty_hash(hash));
    printf("ok test_removed\n");
}

static void test_refused(void) {
    diff(HDR "a.exec=/a\nb.exec=/b\n", NHDR "error: b.svc: protect_home hides exec\na.exec=/a\n");
    assert(nf == 1 && f[0].kind == F_FAIL && strstr(f[0].text, "b.svc: protect_home hides exec"));
    printf("ok test_refused\n");
}

static void test_new_service(void) {
    diff(HDR "a.exec=/a\n", NHDR "a.exec=/a\nb.exec=/b\n");
    assert(nf == 1 && f[0].kind == F_WARN && strstr(f[0].text, "b"));
    assert(is_empty_hash(hash));
    printf("ok test_new_service\n");
}

/* ruling 1 */
static void test_header(void) {
    diff(HDR "a.exec=/a\n", NHDR "a.exec=/a\n");
    assert(nf == 0 && is_empty_hash(hash));
    diff("schema-verify-baseline v1\nbinary_version=1\ncreated=1\ncmdline.root=A\ncmdline.root=B\n"
         "hardening_default=on\nsvc_dir=/s\na.exec=/a\n",
         "schema-verify-baseline v1\nbinary_version=2\ncmdline.root=B\ncmdline.root=A\n"
         "hardening_default=on\nsvc_dir=/s\na.exec=/a\n");
    assert(nf == 1 && f[0].kind == F_CHANGE && strstr(f[0].text, "cmdline.root=  root=A root=B → root=B root=A"));
    diff(HDR "a.exec=/a\n", "schema-verify-baseline v1\nbinary_version=0.4.21\ncmdline.root=UUID=abc\n"
         "cmdline.rd.break\nhardening_default=off\nsvc_dir=/x\nnext_init=known-good\na.exec=/a\n");
    assert(nf == 4 && count(F_CHANGE) == 4);
    assert(find(F_CHANGE, "cmdline.rd.break  unset → rd.break") && find(F_CHANGE, "hardening_default  on → off") &&
           find(F_CHANGE, "svc_dir  /etc/schema-init/services → /x") && find(F_CHANGE, "next_init  unset → known-good"));
    printf("ok test_header\n");
}

/* ruling 2 */
static void test_dotted_names(void) {
    const char *const *keys;
    int nk = svc_serial_keys(&keys), have = 0;
    for (int i = 0; i < nk; i++)
        have += !strcmp(keys[i], "sc.hash") + !strcmp(keys[i], "argv") + !strcmp(keys[i], "syscall_deny");
    assert(have == 3);
    diff(HDR "a.b.sc.hash=111\na.b.sc.mode=deny\nx.sc.exec=/x\n",
         NHDR "a.b.sc.hash=222\na.b.sc.mode=deny\nx.sc.exec=/y\n");
    assert(nf == 2 && count(F_CHANGE) == 2);
    assert(find(F_CHANGE, "sc.hash  111 → 222   1 service: a.b"));
    assert(find(F_CHANGE, "exec  /x → /y   1 service: x.sc"));
    diff(HDR "grp.net.members=a b\n", NHDR "grp.net.members=a\n");
    assert(nf == 1 && f[0].kind == F_CHANGE && strstr(f[0].text, "net") && strstr(f[0].text, "a b → a"));
    printf("ok test_dotted_names\n");
}

/* ruling 3 */
static void test_notes(void) {
    diff(HDR "note: dependency graph: no cycles\na.exec=/a\n",
         NHDR "note: dependency graph: no cycles\nnote: x.svc: masked\n"
         "note: a: protect_system dropped (default would hide exec)\na.exec=/a\n");
    assert(nf == 1 && f[0].kind == F_WARN && strstr(f[0].text, "dropped"));
    assert(is_empty_hash(hash));
    printf("ok test_notes\n");
}

/* ruling 4 */
static void test_errors(void) {
    diff(HDR "error: z.svc: not loaded\na.exec=/a\n",
         NHDR "error: y.svc: not loaded\nerror: z.svc: not loaded\na.exec=/a\n");
    assert(nf == 2 && count(F_FAIL) == 1 && count(F_WARN) == 1);
    assert(find(F_FAIL, "y.svc: not loaded"));
    assert(find(F_WARN, "z.svc: not loaded") && find(F_WARN, "already failing at the last good boot"));
    printf("ok test_errors\n");
}

/* ruling 5 */
static void test_versions(void) {
    diff("schema-verify-baseline v0\nbinary_version=1\na.exec=/a\n", NHDR "a.exec=/b\nb.exec=/b\n");
    assert(nf == 1 && f[0].kind == F_WARN && strstr(f[0].text, "baseline from an older format; refreshed at the next good boot"));
    assert(is_empty_hash(hash));
    assert(diff("schema-verify-baseline v2\na.exec=/a\n", NHDR "a.exec=/a\n") == -1);
    assert(diff("garbage\n", NHDR "a.exec=/a\n") == -1);
    assert(diff("schema-verify-baseline v1x\n", NHDR "a.exec=/a\n") == -1);
    /* a candidate that printed usage instead of a snapshot can't pass */
    diff(HDR "a.exec=/a\n", "usage: schema-init ...\n");
    assert(count(F_FAIL) == 1);
    printf("ok test_versions\n");
}

/* ruling 7 */
static void test_hash_values_only(void) {
    char h1[65], h2[65], h3[65];
    diff(HDR "a.private_tmp=0\tsrc=default\nb.exec=/b\n", NHDR "a.private_tmp=1\tsrc=default\nb.exec=/c\n");
    memcpy(h1, hash, 65);
    assert(count(F_CHANGE) == 2);
    diff(HDR "a.private_tmp=0\tsrc=default\nb.exec=/b\n", NHDR "a.private_tmp=1\tsrc=explicit\nb.exec=/c\n");
    memcpy(h2, hash, 65);
    assert(!strcmp(h1, h2));
    diff(HDR "a.private_tmp=0\tsrc=default\nb.exec=/b\n", NHDR "a.private_tmp=1\tsrc=default\nb.exec=/d\n");
    memcpy(h3, hash, 65);
    assert(strcmp(h1, h3));
    printf("ok test_hash_values_only\n");
}

static char *mkdir_t(const char *tag) {
    static char p[4200];
    snprintf(p, sizeof p, "%s/vd-%s-XXXXXX", tmp, tag);
    assert(mkdtemp(p));
    return p;
}

static void put(const char *d, const char *name, const char *body) {
    char p[4300];
    snprintf(p, sizeof p, "%s/%s", d, name);
    FILE *fp = fopen(p, "w");
    assert(fp);
    fputs(body, fp);
    fclose(fp);
}

static void test_state(void) {
    char *d = mkdir_t("state"), *b = NULL;
    assert(baseline_state(d, &b) == BASE_NEVER && !b);
    put(d, "established", "");
    assert(baseline_state(d, &b) == BASE_LOST && !b);
    put(d, "baseline", "schema-verify-baseline v2\n");
    assert(baseline_state(d, &b) == BASE_LOST && !b);
    put(d, "baseline", "nonsense\n");
    assert(baseline_state(d, &b) == BASE_LOST && !b);
    put(d, "baseline", HDR "a.exec=/a\n");
    assert(baseline_state(d, &b) == BASE_OK && b && !strcmp(b, HDR "a.exec=/a\n"));
    free(b);
    b = NULL;
    /* older: readable, the diff says so */
    put(d, "baseline", "schema-verify-baseline v0\n");
    assert(baseline_state(d, &b) == BASE_OK && b);
    free(b);
    printf("ok test_state\n");
}

static void test_accept(void) {
    char *d = mkdir_t("accept"), p[4300];
    struct stat st;
    diff(HDR "a.exec=/a\n", NHDR "a.exec=/b\n");
    assert(accepted_matches(d, hash) == 0);
    assert(accept_write(d, hash) == 0);
    snprintf(p, sizeof p, "%s/accepted", d);
    assert(lstat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600);
    snprintf(p, sizeof p, "%s/accepted.tmp", d);
    assert(access(p, F_OK) < 0);
    assert(accepted_matches(d, hash) == 1);
    diff(HDR "a.exec=/a\nb.exec=/b\n", NHDR "a.exec=/b\nb.exec=/c\n");
    assert(accepted_matches(d, hash) == 0);
    printf("ok test_accept\n");
}

int main(int argc, char **argv) {
    snprintf(tmp, sizeof tmp, "%s", argc > 1 ? argv[1] : "/tmp");
    test_1118();
    test_argv_change();
    test_source_only();
    test_removed();
    test_refused();
    test_new_service();
    test_header();
    test_dotted_names();
    test_notes();
    test_errors();
    test_versions();
    test_hash_values_only();
    test_state();
    test_accept();
    printf("verify_diff: all ok\n");
    return 0;
}
