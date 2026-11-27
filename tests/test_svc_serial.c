#include "../service.h"
#include "../svc_serial.h"
#include "../sha256.h"
#include "../svc_keys.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-serial-XXXXXX";

static void load(const char *file, const char *body, service_t *s) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    memset(s, 0, sizeof *s);
    assert(service_load_one(p, s) == 0);
    unlink(p);
}

/* the serialized text of S, malloc'd */
static char *ser(const service_t *s) {
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    assert(svc_serialize(s, m) > 0);
    fclose(m);
    return buf;
}

/* the line of TEXT starting with PREFIX, copied into OUT */
static int line_of(const char *text, const char *prefix, char *out, size_t n) {
    size_t pl = strlen(prefix);
    for (const char *p = text; *p; ) {
        size_t l = strcspn(p, "\n");
        if (l >= pl && !strncmp(p, prefix, pl)) {
            snprintf(out, n, "%.*s", (int)l, p);
            return 1;
        }
        p += l + (p[l] == '\n');
    }
    return 0;
}

static void test_args_change(void) {
    static service_t a, b;
    char la[512], lb[512];
    load("x.svc", "name=x\nexec=/usr/bin/mount\nargs=/home\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/mount\nargs=/tmp\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(strcmp(ta, tb) != 0);
    assert(line_of(ta, "x.argv=", la, sizeof la) && line_of(tb, "x.argv=", lb, sizeof lb));
    assert(strcmp(la, lb) != 0);
    free(ta); free(tb);
    printf("ok test_args_change\n");
}

static void test_exec_pre_change(void) {
    static service_t a, b;
    load("x.svc", "name=x\nexec=/usr/bin/true\nexec_pre=/usr/bin/mkdir -p /run/a\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\nexec_pre=/usr/bin/mkdir -p /run/b\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(strcmp(ta, tb) != 0);
    free(ta); free(tb);
    printf("ok test_exec_pre_change\n");
}

static void test_source_only(void) {
    static service_t a, b;
    char la[512], lb[512];
    service_set_hardening_default(1);
    load("x.svc", "name=x\nexec=/usr/bin/true\nno_new_privs=1\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &b);
    service_set_hardening_default(0);
    char *ta = ser(&a), *tb = ser(&b);
    assert(line_of(ta, "x.no_new_privs=1\t", la, sizeof la));
    assert(line_of(tb, "x.no_new_privs=1\t", lb, sizeof lb));
    assert(!strcmp(la, "x.no_new_privs=1\tsrc=explicit"));
    assert(!strcmp(lb, "x.no_new_privs=1\tsrc=default"));
    free(ta); free(tb);
    printf("ok test_source_only\n");
}

static void test_escape(void) {
    static service_t a;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\nargs=a b\targ\\x\n", &a);
    char *t = ser(&a);
    assert(line_of(t, "x.argv=", l, sizeof l));
    assert(!strcmp(l, "x.argv=/usr/bin/true a\\sb\\targ\\\\x"));
    free(t);
    printf("ok test_escape\n");
}

static void test_enum_out_of_range(void) {
    static service_t a;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\nlimit_nofile=1024:4096\nstate_dir=foo\n", &a);
    a.restart_policy = 99;
    a.kill_mode = 7;
    a.cpuset_partition = 5;
    a.priority = 9;
    char *t = ser(&a);
    assert(line_of(t, "x.restart=", l, sizeof l) && !strcmp(l, "x.restart=?99"));
    assert(line_of(t, "x.kill_mode=", l, sizeof l) && !strcmp(l, "x.kill_mode=?7"));
    assert(line_of(t, "x.cpuset_partition=", l, sizeof l) && !strcmp(l, "x.cpuset_partition=?5"));
    assert(line_of(t, "x.priority=", l, sizeof l) && !strcmp(l, "x.priority=?9"));
    assert(line_of(t, "x.limit_nofile=", l, sizeof l) && !strcmp(l, "x.limit_nofile=1024:4096"));
    assert(line_of(t, "x.state_dir=", l, sizeof l) && !strcmp(l, "x.state_dir=foo"));
    free(t);
    printf("ok test_enum_out_of_range\n");
}

static void test_sorted_stable(void) {
    static service_t tab[2];
    load("y.svc", "name=y\nexec=/usr/bin/true\ndep=x\nsyscall_filter=@system-service\n", &tab[0]);
    load("x.svc", "name=x\nexec=/usr/bin/false\nenv=A=1\n", &tab[1]);
    char *b1 = NULL, *b2 = NULL;
    size_t n1 = 0, n2 = 0;
    FILE *m = open_memstream(&b1, &n1);
    int lines = svc_serialize_all(tab, 2, m);
    fclose(m);
    m = open_memstream(&b2, &n2);
    assert(svc_serialize_all(tab, 2, m) == lines);
    fclose(m);
    assert(lines > 20 && n1 == n2 && !memcmp(b1, b2, n1));
    int count = 0;
    for (char *p = b1, *prev = NULL; *p; ) {
        char *nl = strchr(p, '\n');
        assert(nl);
        *nl = '\0';
        if (prev) assert(strcmp(prev, p) < 0);
        prev = p;
        p = nl + 1;
        count++;
    }
    assert(count == lines);
    char l[512];
    assert(line_of(b2, "y.sc.count=", l, sizeof l) && strcmp(l, "y.sc.count=0"));
    assert(line_of(b2, "y.sc.hash=", l, sizeof l) && strlen(l) == strlen("y.sc.hash=") + 64);
    free(b1); free(b2);
    printf("ok test_sorted_stable\n");
}

static void test_sha256(void) {
    char h[65];
    sha256_hex("abc", 3, h);
    assert(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256_hex("", 0, h);
    assert(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    assert(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    printf("ok test_sha256\n");
}

/* an empty item must not vanish into its neighbours or into an empty list */
static void test_empty_item(void) {
    static service_t a, b;
    char la[512], lb[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &a);
    b = a;
    a.listen_count = 2; strcpy(a.listen[0], ""); strcpy(a.listen[1], "fifo:/run/a");
    b.listen_count = 1; strcpy(b.listen[0], "fifo:/run/a");
    char *ta = ser(&a), *tb = ser(&b);
    assert(line_of(ta, "x.listen=", la, sizeof la) && line_of(tb, "x.listen=", lb, sizeof lb));
    assert(strcmp(la, lb) != 0);
    free(ta); free(tb);
    a.listen_count = 1; strcpy(a.listen[0], "");
    b.listen_count = 0;
    ta = ser(&a); tb = ser(&b);
    assert(line_of(ta, "x.listen=", la, sizeof la) && line_of(tb, "x.listen=", lb, sizeof lb));
    assert(strcmp(la, lb) != 0);
    free(ta); free(tb);
    printf("ok test_empty_item\n");
}

/* the effective syscall_deny, explicit or defaulted (the 11-18 shape) */
static void test_syscall_deny_label(void) {
    static service_t a;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\nsyscall_deny=default\n", &a);
    char *t = ser(&a);
    assert(line_of(t, "x.syscall_deny=", l, sizeof l) && !strcmp(l, "x.syscall_deny=default\tsrc=explicit"));
    free(t);
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &a);
    t = ser(&a);
    assert(line_of(t, "x.syscall_deny=", l, sizeof l) && !strcmp(l, "x.syscall_deny=-"));
    free(t);
    a.hard_default |= HARD_SD;
    t = ser(&a);
    assert(line_of(t, "x.syscall_deny=", l, sizeof l) && !strcmp(l, "x.syscall_deny=default\tsrc=default"));
    free(t);
    printf("ok test_syscall_deny_label\n");
}

/* the resolved set, not the spelling: syscall_deny=0 vs default must move sc.hash */
static void test_sc_hash_tracks_set(void) {
    static service_t a, b;
    char la[512], lb[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\nsyscall_deny=0\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\nsyscall_deny=default\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(line_of(ta, "x.sc.hash=", la, sizeof la) && line_of(tb, "x.sc.hash=", lb, sizeof lb));
    assert(strcmp(la, lb) != 0);
    assert(line_of(ta, "x.sc.count=", la, sizeof la) && !strcmp(la, "x.sc.count=0"));
    assert(line_of(tb, "x.sc.count=", lb, sizeof lb) && strcmp(lb, "x.sc.count=0"));
    free(ta); free(tb);
    printf("ok test_sc_hash_tracks_set\n");
}

/* after=/before= lines only when set: older baselines have none for any service */
static void test_order_lines(void) {
    static service_t a, b;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\nafter=p\nafter=q\nbefore=r\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(!strstr(ta, "x.after=") && !strstr(ta, "x.before="));
    assert(line_of(tb, "x.after=", l, sizeof l) && !strcmp(l, "x.after=p q"));
    assert(line_of(tb, "x.before=", l, sizeof l) && !strcmp(l, "x.before=r"));
    free(ta); free(tb);
    printf("ok test_order_lines\n");
}

/* a key added since the last baseline: written only when set, or verify sees a
 * change on every service and refuses the re-exec (v0.4.35 on BlakPi) */
static void test_dir_owner_only_when_set(void) {
    static service_t a, b;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\ndir_owner=chrony:adm\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(!strstr(ta, "x.dir_owner"));
    assert(line_of(tb, "x.dir_owner=", l, sizeof l) && !strcmp(l, "x.dir_owner=chrony:adm"));
    free(ta); free(tb);
    printf("ok test_dir_owner_only_when_set\n");
}

static void test_bounding_caps_only_when_set(void) {
    static service_t a, b;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\n", &a);
    load("x.svc", "name=x\nexec=/usr/bin/true\nbounding_caps=~CAP_SYS_ADMIN\n", &b);
    char *ta = ser(&a), *tb = ser(&b);
    assert(!strstr(ta, "x.bounding_caps"));
    assert(line_of(tb, "x.bounding_caps=", l, sizeof l) && !strcmp(l, "x.bounding_caps=0x0000000000200000"));
    free(ta); free(tb);
    printf("ok test_bounding_caps_only_when_set\n");
}

/* stdout=, stderr=, ignore_sigpipe=, keyring_mode=: written only off their default,
 * parsed back to the same; a bad value refuses the load */
static void test_stdio_keyring(void) {
    static service_t a, b, c;
    char l[512];
    load("x.svc", "name=x\nexec=/usr/bin/true\nstdout=log\nkeyring_mode=inherit\nignore_sigpipe=1\n", &a);
    char *ta = ser(&a);
    assert(!strstr(ta, "x.stdout") && !strstr(ta, "x.stderr") && !strstr(ta, "x.keyring_mode") &&
           !strstr(ta, "x.ignore_sigpipe"));
    load("x.svc", "name=x\nexec=/usr/bin/true\nstdout=null\nstderr=append:/var/log/x.err\n"
         "keyring_mode=shared\nignore_sigpipe=0\n", &b);
    char *tb = ser(&b);
    assert(line_of(tb, "x.stdout=", l, sizeof l) && !strcmp(l, "x.stdout=null"));
    assert(line_of(tb, "x.stderr=", l, sizeof l) && !strcmp(l, "x.stderr=append:/var/log/x.err"));
    assert(line_of(tb, "x.keyring_mode=", l, sizeof l) && !strcmp(l, "x.keyring_mode=shared"));
    assert(line_of(tb, "x.ignore_sigpipe=", l, sizeof l) && !strcmp(l, "x.ignore_sigpipe=0"));
    assert(b.out_mode[0] == SVC_OUT_NULL && b.out_mode[1] == SVC_OUT_APPEND && b.sigpipe_dfl == 1);
    /* stderr=log is its own setting: the log even when stdout goes elsewhere */
    load("x.svc", "name=x\nexec=/usr/bin/true\nstdout=truncate:/tmp/o\nstderr=log\n", &c);
    assert(c.out_mode[0] == SVC_OUT_TRUNCATE && !strcmp(c.out_path[0], "/tmp/o") && c.out_mode[1] == SVC_OUT_LOG);
    /* only an explicit off turns SIGPIPE back on */
    load("x.svc", "name=x\nexec=/usr/bin/true\nignore_sigpipe=yes\n", &c);
    assert(!c.sigpipe_dfl);
    load("x.svc", "name=x\nexec=/usr/bin/true\nignore_sigpipe=no\n", &c);
    assert(c.sigpipe_dfl);
    const char *bad[] = { "stdout=inherit\n", "stdout=file:rel\n", "stderr=journal\n", "keyring_mode=session\n" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char p[300], body[128];
        snprintf(p, sizeof p, "%s/bad.svc", dir);
        snprintf(body, sizeof body, "name=x\nexec=/usr/bin/true\n%s", bad[i]);
        FILE *f = fopen(p, "w");
        fputs(body, f);
        fclose(f);
        memset(&c, 0, sizeof c);
        assert(service_load_one(p, &c) != 0);
        unlink(p);
    }
    free(ta); free(tb);
    printf("ok test_stdio_keyring\n");
}

/* svc_keys[] is what verify splits lines on: it must be exactly what the emitters write */
static void test_keys_table(void) {
    const char *const *em;
    int ne = svc_serial_keys(&em), bad = 0;
    for (int i = 0; i < ne; i++) {
        int k = 0;
        while (k < svc_keys_n && strcmp(svc_keys[k], em[i])) k++;
        if (k == svc_keys_n) { printf("FAIL emitted key %s missing from svc_keys[]\n", em[i]); bad = 1; }
    }
    for (int k = 0; k < svc_keys_n; k++) {
        int i = 0;
        while (i < ne && strcmp(svc_keys[k], em[i])) i++;
        if (i == ne) { printf("FAIL svc_keys[] key %s is never emitted\n", svc_keys[k]); bad = 1; }
    }
    assert(!bad && ne == svc_keys_n);
    printf("ok test_keys_table\n");
}

int main(void) {
    assert(mkdtemp(dir));
    test_keys_table();
    test_order_lines();
    test_dir_owner_only_when_set();
    test_bounding_caps_only_when_set();
    test_stdio_keyring();
    test_args_change();
    test_exec_pre_change();
    test_source_only();
    test_escape();
    test_enum_out_of_range();
    test_sorted_stable();
    test_sha256();
    test_empty_item();
    test_syscall_deny_label();
    test_sc_hash_tracks_set();
    rmdir(dir);
    return 0;
}
