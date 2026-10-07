#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[64];

static void put(const char *rel, const char *body) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void mk(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    assert(mkdir(p, 0755) == 0);
}

static void drop(service_t *s) {
    for (int i = 1; i < MAX_ARGV; i++) free(s->argv[i]);
    for (int i = 0; i < s->env_count; i++) free(s->envp[i]);
    for (int i = 0; i < s->landlock_count; i++) free(s->landlock[i]);
}

static int load(const char *rel, service_t *s) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    return service_load_one(p, s);
}

static uint32_t old_hash(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "r");
    uint32_t h = 2166136261u;
    int c;
    while ((c = fgetc(f)) != EOF) { h ^= (unsigned char)c; h *= 16777619u; }
    fclose(f);
    return h;
}

int main(void) {
    service_t s;
    uint32_t h0, h1;
    snprintf(dir, sizeof dir, "/tmp/schema-dropinXXXXXX");
    assert(mkdtemp(dir));

    /* no drop-ins: hash identical to the pre-drop-in algorithm */
    put("a.svc", "exec=/bin/a\nargs=-1\nargs=-2\nenv=A=1\ndep=x\nmem_limit=100\nno_restart=1\n");
    assert(load("a.svc", &s) == 0);
    assert(s.content_hash == old_hash("a.svc"));
    h0 = s.content_hash;
    drop(&s);

    /* scalar override, list append, flag clear, order 10 < 50, junk ignored */
    mk("a.svc.d");
    put("a.svc.d/50-late.conf", "mem_limit=300\nargs=-4\n");
    put("a.svc.d/10-early.conf", "mem_limit=200\nargs=-3\nenv=B=2\ndep=y\nno_restart=0\n");
    put("a.svc.d/20-skip.txt", "mem_limit=999\n");
    put("a.svc.d/.#30-lock.conf", "mem_limit=999\n");
    put("a.svc.d/40-x.conf.swp", "mem_limit=999\n");
    assert(load("a.svc", &s) == 0);
    assert(s.mem_limit_mb == 300);
    assert(strcmp(s.argv[1], "-1") == 0 && strcmp(s.argv[2], "-2") == 0 &&
           strcmp(s.argv[3], "-3") == 0 && strcmp(s.argv[4], "-4") == 0 && !s.argv[5]);
    assert(s.env_count == 2 && strcmp(s.envp[1], "B=2") == 0);
    assert(strcmp(s.dep_name[0], "x") == 0 && strcmp(s.dep_name[1], "y") == 0);
    assert(!(s.flags & SVC_NO_RESTART));
    assert(s.content_hash != h0);
    h1 = s.content_hash;
    drop(&s);

    /* edit, remove and rename each change the hash */
    put("a.svc.d/50-late.conf", "mem_limit=301\nargs=-4\n");
    assert(load("a.svc", &s) == 0 && s.content_hash != h1);
    drop(&s);
    put("a.svc.d/50-late.conf", "mem_limit=300\nargs=-4\n");
    assert(load("a.svc", &s) == 0 && s.content_hash == h1);
    drop(&s);
    {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/a.svc.d/50-late.conf", dir);
        snprintf(b, sizeof b, "%s/a.svc.d/60-late.conf", dir);
        assert(rename(a, b) == 0);
        assert(load("a.svc", &s) == 0 && s.content_hash != h1);
        drop(&s);
        assert(unlink(b) == 0);
        assert(load("a.svc", &s) == 0 && s.content_hash != h1 && s.mem_limit_mb == 200);
        drop(&s);
    }

    /* empty assignment resets each list; exec= keeps args */
    put("b.svc", "exec=/bin/b\nargs=-1\nargs=-2\nenv=A=1\ndep=x\n");
    mk("b.svc.d");
    put("b.svc.d/10.conf", "exec=/bin/b2\nargs=\nargs=-n\nenv=\ndep= \ndep=z\n");
    assert(load("b.svc", &s) == 0);
    assert(strcmp(s.exec, "/bin/b2") == 0 && s.argv[0] == s.exec);
    assert(strcmp(s.argv[1], "-n") == 0 && !s.argv[2]);
    assert(s.env_count == 0);
    assert(strcmp(s.dep_name[0], "z") == 0 && !s.dep_name[1][0]);
    drop(&s);
    put("b.svc.d/10.conf", "exec=/bin/b3\n");
    assert(load("b.svc", &s) == 0);
    assert(s.argv[0] == s.exec && strcmp(s.argv[1], "-1") == 0 &&
           strcmp(s.argv[2], "-2") == 0 && !s.argv[3]);
    drop(&s);

    /* no_new_privs=0 in a drop-in clears what the base set */
    put("n.svc", "exec=/bin/n\nno_new_privs=1\n");
    mk("n.svc.d");
    put("n.svc.d/10.conf", "no_new_privs=0\n");
    assert(load("n.svc", &s) == 0 && !(s.flags & SVC_NO_NEW_PRIVS));
    drop(&s);
    put("n.svc.d/10.conf", "no_new_privs=1\n");
    put("n.svc", "exec=/bin/n\n");
    assert(load("n.svc", &s) == 0 && (s.flags & SVC_NO_NEW_PRIVS));
    drop(&s);

    /* an empty landlock_ro= drops only the ro paths; rw ones and their bits stay */
    put("l.svc", "exec=/usr/bin/l\nlandlock_ro=/usr\nlandlock_rw=/var/lib/l\nlandlock_ro=/etc\n");
    mk("l.svc.d");
    put("l.svc.d/10.conf", "landlock_ro=\nlandlock_ro=/usr/bin\n");
    assert(load("l.svc", &s) == 0 && s.landlock_count == 2);
    assert(strcmp(s.landlock[0], "/var/lib/l") == 0 && strcmp(s.landlock[1], "/usr/bin") == 0);
    assert(s.landlock_rw == 1);
    drop(&s);
    put("l.svc.d/10.conf", "landlock_ro=\n");
    assert(load("l.svc", &s) == -1);
    drop(&s);

    /* base files keep their old meaning: empty args= is an argument, =0 is a no-op */
    put("c.svc", "exec=/bin/c\nno_restart=1\nno_restart=0\nargs=\n");
    assert(load("c.svc", &s) == 0);
    assert((s.flags & SVC_NO_RESTART) && s.argv[1] && s.argv[1][0] == '\0');
    drop(&s);

    /* name= in a drop-in, or a bad line, rejects the service */
    put("d.svc", "exec=/bin/d\n");
    mk("d.svc.d");
    put("d.svc.d/10.conf", "name=other\n");
    assert(load("d.svc", &s) == -1);
    put("d.svc.d/10.conf", "args=-a\nkeep_caps=CAP_NOPE\n");
    assert(load("d.svc", &s) == -1);
    put("d.svc.d/10.conf", "exec=/bin/has space\n");
    assert(load("d.svc", &s) == -1);

    /* symlink to a regular file is followed; a directory named *.conf is skipped */
    put("shared.conf", "mem_limit=64\n");
    put("e.svc", "exec=/bin/e\n");
    mk("e.svc.d");
    {
        char t[512], l[512];
        snprintf(t, sizeof t, "%s/shared.conf", dir);
        snprintf(l, sizeof l, "%s/e.svc.d/10-shared.conf", dir);
        assert(symlink(t, l) == 0);
    }
    mk("e.svc.d/20-dir.conf");
    assert(load("e.svc", &s) == 0 && s.mem_limit_mb == 64);
    drop(&s);

    /* template dir then instance dir */
    put("m@a.svc", "exec=/bin/m\n");
    put("m@b.svc", "exec=/bin/m\n");
    mk("m@.svc.d");
    mk("m@a.svc.d");
    put("m@.svc.d/50.conf", "mem_limit=10\ncpu_limit=5\n");
    put("m@a.svc.d/10.conf", "mem_limit=20\n");
    assert(load("m@a.svc", &s) == 0);
    assert(s.mem_limit_mb == 20 && s.cpu_limit_pct == 5 && strcmp(s.instance, "a") == 0);
    drop(&s);
    assert(load("m@b.svc", &s) == 0 && s.mem_limit_mb == 10);
    drop(&s);

    /* orphan .svc.d with no .svc creates nothing; services_load sees the drop-ins too */
    mk("ghost.svc.d");
    put("ghost.svc.d/10.conf", "exec=/bin/ghost\n");
    {
        static service_t table[16];
        int n = services_load(dir, table, 16);
        int found_a = 0;
        for (int i = 0; i < n; i++) {
            assert(strcmp(table[i].name, "ghost") != 0);
            if (strcmp(table[i].name, "a") == 0) found_a = table[i].mem_limit_mb == 200;
            drop(&table[i]);
        }
        assert(found_a);
    }

    put("st.svc", "name=st\nexec=/bin/st\n");
    assert(load("st.svc", &s) == 0 && s.stop_timeout_sec == 0);
    drop(&s);
    mk("st.svc.d");
    put("st.svc.d/50-stop.conf", "stop_timeout_sec=20\n");
    assert(load("st.svc", &s) == 0 && s.stop_timeout_sec == 20);
    drop(&s);
    put("st.svc.d/50-stop.conf", "stop_timeout_sec=9999\n");
    assert(load("st.svc", &s) == 0 && s.stop_timeout_sec == 0);
    drop(&s);

    printf("all svc-dropins tests passed\n");
    return 0;
}
