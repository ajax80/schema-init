#include "../service.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char path[300];

static int load(const char *body, service_t *svc) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    memset(svc, 0, sizeof *svc);
    return service_load_one(path, svc);
}

static int died(service_t *s, int code, int sig) {
    s->exit_status = sig ? -1 : code;
    s->term_signal = sig;
    s->has_exited = 1;
    return service_death_class(s);
}

int main(void) {
    char dir[] = "/tmp/schema-restart-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/r.svc", dir);
    service_t s;

    /* default and each value */
    assert(load("name=r\nexec=/bin/true\n", &s) == 0 && s.restart_policy == RESTART_ALWAYS && !(s.flags & SVC_NO_RESTART));
    assert(load("name=r\nexec=/bin/true\nrestart=on-failure\n", &s) == 0 && s.restart_policy == RESTART_ON_FAILURE);
    assert(load("name=r\nexec=/bin/true\nrestart=on-watchdog\n", &s) == 0 && s.restart_policy == RESTART_ON_WATCHDOG);
    assert(load("name=r\nexec=/bin/true\nrestart=no\n", &s) == 0 && s.restart_policy == RESTART_NO && (s.flags & SVC_NO_RESTART));
    assert(load("name=r\nexec=/bin/true\nrestart=sometimes\n", &s) == -1);
    /* last of restart= / no_restart= wins */
    assert(load("name=r\nexec=/bin/true\nno_restart=1\nrestart=on-failure\n", &s) == 0 &&
           s.restart_policy == RESTART_ON_FAILURE && !(s.flags & SVC_NO_RESTART));
    assert(load("name=r\nexec=/bin/true\nrestart=on-failure\nno_restart=1\n", &s) == 0 &&
           s.restart_policy == RESTART_NO && (s.flags & SVC_NO_RESTART));

    /* classification: systemd's clean signals, success_exit_status= */
    assert(load("name=r\nexec=/bin/true\nsuccess_exit_status=3 SIGUSR1\nsuccess_exit_status=143\n", &s) == 0);
    assert(died(&s, 0, 0) == DEATH_CLEAN);
    assert(died(&s, 3, 0) == DEATH_CLEAN);
    assert(died(&s, 143, 0) == DEATH_CLEAN);
    assert(died(&s, 4, 0) == DEATH_EXIT);
    assert(died(&s, 0, SIGTERM) == DEATH_CLEAN);
    assert(died(&s, 0, SIGHUP) == DEATH_CLEAN);
    assert(died(&s, 0, SIGUSR1) == DEATH_CLEAN);
    assert(died(&s, 0, SIGSEGV) == DEATH_SIGNAL);
    s.wd_fired = 1;
    assert(died(&s, 0, SIGABRT) == DEATH_WATCHDOG);
    s.wd_fired = 0;
    assert(load("name=r\nexec=/bin/true\nsuccess_exit_status=256\n", &s) == -1);
    assert(load("name=r\nexec=/bin/true\nsuccess_exit_status=BOGUS\n", &s) == -1);

    /* systemd's Restart= table */
    static const struct { int pol; int want[5]; } t[] = {
        /*                       CLEAN EXIT SIGNAL WATCHDOG TIMEOUT */
        { RESTART_ALWAYS,      { 1,    1,   1,     1,       1 } },
        { RESTART_NO,          { 0,    0,   0,     0,       0 } },
        { RESTART_ON_SUCCESS,  { 1,    0,   0,     0,       0 } },
        { RESTART_ON_FAILURE,  { 0,    1,   1,     1,       1 } },
        { RESTART_ON_ABNORMAL, { 0,    0,   1,     1,       1 } },
        { RESTART_ON_ABORT,    { 0,    0,   1,     0,       0 } },
        { RESTART_ON_WATCHDOG, { 0,    0,   0,     1,       0 } },
    };
    memset(&s, 0, sizeof s);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        s.restart_policy = (uint8_t)t[i].pol;
        for (int d = DEATH_CLEAN; d <= DEATH_TIMEOUT; d++)
            assert(service_restart_wanted(&s, d) == t[i].want[d]);
    }

    /* restart_prevent_exit_status= beats restart=always, but not a timeout/watchdog */
    assert(load("name=r\nexec=/bin/true\nrestart_prevent_exit_status=6 SIGKILL\n", &s) == 0);
    assert(!service_restart_wanted(&s, died(&s, 6, 0)));
    assert(service_restart_wanted(&s, died(&s, 7, 0)));
    assert(!service_restart_wanted(&s, died(&s, 0, SIGKILL)));
    assert(service_restart_wanted(&s, DEATH_TIMEOUT));

    /* drop-in: an empty value clears each key */
    {
        char dd[400], dp[450];
        snprintf(dd, sizeof dd, "%s.d", path);
        assert(mkdir(dd, 0755) == 0);
        snprintf(dp, sizeof dp, "%s/10.conf", dd);
        FILE *f = fopen(dp, "w");
        fputs("restart=\nsuccess_exit_status=\nrestart_prevent_exit_status=\n", f);
        fclose(f);
        assert(load("name=r\nexec=/bin/true\nrestart=no\nsuccess_exit_status=3\nrestart_prevent_exit_status=4\n", &s) == 0);
        assert(s.restart_policy == RESTART_ALWAYS && !(s.flags & SVC_NO_RESTART));
        assert(died(&s, 3, 0) == DEATH_EXIT && service_restart_wanted(&s, died(&s, 4, 0)));
        unlink(dp);
        rmdir(dd);
    }
    unlink(path);
    rmdir(dir);
    printf("test_svc_restart_policy: ok\n");
    return 0;
}
