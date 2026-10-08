#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int find(const service_t *svc, int res) {
    for (int i = 0; i < svc->rlim_count; i++)
        if (svc->rlim[i].res == res) return i;
    return -1;
}

static int run(service_t *svc) {
    int st;
    assert(service_spawn(svc) == 0);
    assert(waitpid(svc->child_pid, &st, 0) == svc->child_pid && WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void slurp(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    assert(f);
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = '\0';
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/schema-svc-rlimXXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    char p[256], out[256], body[1024], got[512];
    service_t svc;
    int i;

    snprintf(p, sizeof p, "%s/a.svc", dir);
    write_svc(p,
        "name=a\nexec=/bin/true\n"
        "limit_nofile=1024:4096\n"
        "limit_memlock=4G\n"
        "limit_rtprio=70\n"
        "limit_nice=-19\n"
        "limit_core=infinity\n"
        "limit_stack=8M\n"
        "limit_nofile=2048:8192\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.rlim_count == 6);
    i = find(&svc, RLIMIT_NOFILE);
    assert(i >= 0 && svc.rlim[i].cur == 2048 && svc.rlim[i].max == 8192);
    i = find(&svc, RLIMIT_MEMLOCK);
    assert(i >= 0 && svc.rlim[i].cur == 4294967296ULL && svc.rlim[i].max == 4294967296ULL);
    i = find(&svc, RLIMIT_RTPRIO);
    assert(i >= 0 && svc.rlim[i].cur == 70 && svc.rlim[i].max == 70);
    i = find(&svc, RLIMIT_NICE);
    assert(i >= 0 && svc.rlim[i].cur == 39);
    i = find(&svc, RLIMIT_CORE);
    assert(i >= 0 && svc.rlim[i].cur == RLIM_INFINITY && svc.rlim[i].max == RLIM_INFINITY);
    i = find(&svc, RLIMIT_STACK);
    assert(i >= 0 && svc.rlim[i].cur == 8388608);

    snprintf(p, sizeof p, "%s/b.svc", dir);
    write_svc(p, "name=b\nexec=/bin/true\nlimit_nice=39\nlimit_cpu=10:infinity\n");
    assert(service_load_one(p, &svc) == 0);
    i = find(&svc, RLIMIT_NICE);
    assert(i >= 0 && svc.rlim[i].cur == 39);
    i = find(&svc, RLIMIT_CPU);
    assert(i >= 0 && svc.rlim[i].cur == 10 && svc.rlim[i].max == RLIM_INFINITY);

    static const char *bad[] = {
        "limit_nofile=abc", "limit_nofile=10X", "limit_nofile=-5", "limit_nofile=8:4",
        "limit_nofile=infinity:10", "limit_nice=-21", "limit_nice=+20", "limit_rtprio=",
        "limit_memlock=4GB",
    };
    for (size_t b = 0; b < sizeof bad / sizeof bad[0]; b++) {
        snprintf(body, sizeof body, "name=c\nexec=/bin/true\n%s\n", bad[b]);
        write_svc(p, body);
        int r = service_load_one(p, &svc);
        if (strcmp(bad[b], "limit_rtprio=") == 0)
            assert(r == 0 && svc.rlim_count == 0);
        else
            assert(r == -1);
    }

    /* a drop-in replaces one limit and an empty value removes another */
    char dd[300];
    snprintf(p, sizeof p, "%s/d.svc", dir);
    write_svc(p, "name=d\nexec=/bin/true\nlimit_nofile=1024\nlimit_rtprio=70\n");
    snprintf(dd, sizeof dd, "%s/d.svc.d", dir);
    assert(mkdir(dd, 0755) == 0);
    snprintf(dd, sizeof dd, "%s/d.svc.d/10.conf", dir);
    write_svc(dd, "limit_nofile=4096\nlimit_rtprio=\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.rlim_count == 1);
    i = find(&svc, RLIMIT_NOFILE);
    assert(i >= 0 && svc.rlim[i].cur == 4096);

    /* the child really runs with them */
    snprintf(out, sizeof out, "%s/limits.out", dir);
    snprintf(p, sizeof p, "%s/e.svc", dir);
    snprintf(body, sizeof body,
        "name=e\nexec=/bin/sh\nargs=-c\nargs=echo $(ulimit -Sn) $(ulimit -Hn) $(ulimit -Sc) $(ulimit -Ss) > %s\n"
        "limit_nofile=512:1024\nlimit_core=0\nlimit_stack=4M\n", out);
    write_svc(p, body);
    assert(service_load_one(p, &svc) == 0);
    assert(run(&svc) == 0);
    slurp(out, got, sizeof got);
    assert(strcmp(got, "512 1024 0 4096\n") == 0);

    /* infinity on nofile means fs.nr_open, as systemd does */
    if (getuid() == 0) {
        unsigned long nr;
        FILE *f = fopen("/proc/sys/fs/nr_open", "r");
        assert(f && fscanf(f, "%lu", &nr) == 1);
        fclose(f);
        snprintf(body, sizeof body,
            "name=e\nexec=/bin/sh\nargs=-c\nargs=ulimit -Hn > %s\nlimit_nofile=infinity\n", out);
        write_svc(p, body);
        assert(service_load_one(p, &svc) == 0);
        assert(run(&svc) == 0);
        slurp(out, got, sizeof got);
        assert(strtoul(got, NULL, 10) == nr);
    } else {
        /* raising a hard limit unprivileged fails, and the start fails with it */
        snprintf(body, sizeof body,
            "name=e\nexec=/bin/true\nlimit_rtprio=99\n");
        write_svc(p, body);
        assert(service_load_one(p, &svc) == 0);
        assert(run(&svc) == 126);
    }

    /* a user that does not exist must not start as root */
    snprintf(p, sizeof p, "%s/f.svc", dir);
    write_svc(p, "name=f\nexec=/bin/true\nuser=schema-no-such-user\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.run_uid == 0 && strcmp(svc.run_user, "schema-no-such-user") == 0);
    assert(run(&svc) == 126);

    /* numeric user= resolves to the account */
    write_svc(p, "name=f\nexec=/bin/true\nuser=65534\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.run_uid == 65534 && svc.run_user[0]);

    if (getuid() == 0) {
        struct passwd *pw = getpwuid(65534);
        assert(pw);
        chmod(dir, 0777);
        snprintf(body, sizeof body,
            "name=f\nexec=/bin/sh\nargs=-c\n"
            "args=echo $(id -u) $HOME $USER $LOGNAME $(ulimit -r) $FOO > %s\n"
            "user=65534\nlimit_rtprio=70\nenv=FOO=kept\nenv=LOGNAME=override\n", out);
        write_svc(p, body);
        unlink(out);
        assert(service_load_one(p, &svc) == 0);
        assert(run(&svc) == 0);
        slurp(out, got, sizeof got);
        char want[256];
        snprintf(want, sizeof want, "65534 %s %s override 70 kept\n", pw->pw_dir, pw->pw_name);
        assert(strcmp(got, want) == 0);
        printf("test_svc_rlimits: root spawn checks ran\n");
    }

    printf("test_svc_rlimits: OK\n");
    return 0;
}
