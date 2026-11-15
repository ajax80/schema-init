#include "../service.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int load(const char *path, const char *body, service_t *svc) {
    write_svc(path, body);
    memset(svc, 0, sizeof *svc);
    return service_load_one(path, svc);
}

static int runner(service_t *svc, pid_t mainpid) {
    int st;
    pid_t p = service_reload_spawn(svc, mainpid);
    assert(p > 0);
    assert(waitpid(p, &st, 0) == p && WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void slurp(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    buf[0] = '\0';
    if (!f) return;
    size_t r = fread(buf, 1, n - 1, f);
    buf[r] = '\0';
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/schema-reload-XXXXXX", p[300], out[300], body[2048], buf[256];
    service_t svc;
    assert(mkdtemp(dir));
    snprintf(p, sizeof p, "%s/s.svc", dir);
    snprintf(out, sizeof out, "%s/out", dir);

    /* exec_reload: two commands in order, $MAINPID and ${MAINPID} expanded, env set */
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\n"
             "exec_reload=/bin/sh -c \"echo a$MAINPID-${MAINPID} >> %s\"\n"
             "exec_reload=/bin/sh -c \"echo env=$MAINPID >> %s\"\n", out, out);
    /* the first line's $MAINPID sits inside sh's own quoted script: the engine
     * replaces it before sh sees it, so both forms come out as the pid */
    assert(load(p, body, &svc) == 0);
    assert(svc.exec_reload_count == 2 && svc.reload_signal == 0);
    assert(runner(&svc, 4242) == 0);
    slurp(out, buf, sizeof buf);
    assert(strcmp(buf, "a4242-4242\nenv=4242\n") == 0);
    unlink(out);

    /* a failing command stops the list, the runner exits non-zero; '-' ignores */
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\nexec_reload=-/bin/false\nexec_reload=/bin/false\n"
             "exec_reload=/bin/sh -c \"echo never >> %s\"\n", out);
    assert(load(p, body, &svc) == 0);
    assert(runner(&svc, 1) != 0);
    slurp(out, buf, sizeof buf);
    assert(buf[0] == '\0');

    /* reload_signal: names, SIG prefix, numbers */
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=HUP\n", &svc) == 0 && svc.reload_signal == SIGHUP);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=SIGUSR1\n", &svc) == 0 && svc.reload_signal == SIGUSR1);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=urg\n", &svc) == 0 && svc.reload_signal == SIGURG);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=10\n", &svc) == 0 && svc.reload_signal == 10);
    /* bad values refuse the service */
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=BOGUS\n", &svc) == -1);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=0\n", &svc) == -1);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=99\n", &svc) == -1);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=KILL\n", &svc) == -1);
    /* reload=1 is reload_signal=HUP; an explicit reload_signal wins in either order */
    assert(load(p, "name=s\nexec=/bin/true\nreload=1\n", &svc) == 0 && svc.reload_signal == SIGHUP);
    assert(load(p, "name=s\nexec=/bin/true\nreload=0\n", &svc) == 0 && svc.reload_signal == 0);
    assert(load(p, "name=s\nexec=/bin/true\nreload=1\nreload_signal=USR2\n", &svc) == 0 && svc.reload_signal == SIGUSR2);
    assert(load(p, "name=s\nexec=/bin/true\nreload_signal=USR2\nreload=1\n", &svc) == 0 && svc.reload_signal == SIGUSR2);
    /* a drop-in's empty reload_signal= clears it (and reload=1's HUP), as for the sibling lists */
    {
        char dd[400], dp[450];
        snprintf(dd, sizeof dd, "%s.d", p);
        assert(mkdir(dd, 0755) == 0);
        snprintf(dp, sizeof dp, "%s/10-off.conf", dd);
        write_svc(dp, "reload_signal=\nexec_reload=\n");
        assert(load(p, "name=s\nexec=/bin/true\nreload_signal=HUP\nexec_reload=/bin/true\n", &svc) == 0);
        assert(svc.reload_signal == 0 && svc.exec_reload_count == 0);
        unlink(dp);
        rmdir(dd);
    }
    /* exec_reload= needs an absolute command */
    assert(load(p, "name=s\nexec=/bin/true\nexec_reload=true\n", &svc) == -1);

    unlink(p);
    rmdir(dir);
    printf("test_svc_reload: ok\n");
    return 0;
}
