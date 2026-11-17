#include "../service.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static char dir[] = "/tmp/schema-sched-XXXXXX";
static char path[300];

static int load(const char *keys, service_t *s) {
    char body[512];
    FILE *f = fopen(path, "w");
    assert(f);
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    fputs(body, f);
    fclose(f);
    memset(s, 0, sizeof *s);
    return service_load_one(path, s);
}

/* spawn, let the child write what it sees, return its exit status */
static int spawn_probe(const char *keys, const char *probe, char *out, size_t n) {
    char body[1024], of[320];
    int st;
    static service_t s;
    snprintf(of, sizeof of, "%s/out", dir);
    unlink(of);
    snprintf(body, sizeof body, "exec=/bin/sh\nargs=-c\nargs=%s > %s\noneshot=1\n%s", probe, of, keys);
    FILE *f = fopen(path, "w");
    fprintf(f, "name=t\n%s", body);
    fclose(f);
    memset(&s, 0, sizeof s);
    assert(service_load_one(path, &s) == 0);
    assert(service_spawn(&s) == 0);
    assert(waitpid(s.child_pid, &st, 0) == s.child_pid && WIFEXITED(st));
    out[0] = 0;
    f = fopen(of, "r");
    if (f) {
        if (!fgets(out, (int)n, f)) out[0] = 0;
        fclose(f);
        out[strcspn(out, "\n")] = 0;
    }
    return WEXITSTATUS(st);
}

int main(void) {
    static service_t s;
    char got[128];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);

    assert(load("", &s) == 0 && !s.nice_set && s.io_class == -1 && s.io_prio == -1 && s.sched_policy == -1);
    assert(load("nice=-20\n", &s) == 0 && s.nice_set && s.nice_val == -20);
    assert(load("nice=19\n", &s) == 0 && s.nice_val == 19);
    assert(load("io_class=idle\n", &s) == 0 && s.io_class == 3);
    assert(load("io_class=realtime\nio_priority=0\n", &s) == 0 && s.io_class == 1 && s.io_prio == 0);
    assert(load("io_class=best-effort\n", &s) == 0 && s.io_class == 2);
    assert(load("io_class=none\n", &s) == 0 && s.io_class == 0);
    assert(load("io_priority=7\n", &s) == 0 && s.io_prio == 7);
    assert(load("sched_policy=fifo\n", &s) == 0 && s.sched_policy == SCHED_FIFO && s.sched_prio == 1);
    assert(load("sched_policy=rr\nsched_priority=99\n", &s) == 0 && s.sched_policy == SCHED_RR && s.sched_prio == 99);
    assert(load("sched_policy=batch\n", &s) == 0 && s.sched_policy == SCHED_BATCH);
    assert(load("sched_policy=idle\n", &s) == 0 && s.sched_policy == SCHED_IDLE);
    assert(load("sched_policy=other\n", &s) == 0 && s.sched_policy == SCHED_OTHER);
    {
        const char *bad[] = {
            "nice=20\n", "nice=-21\n", "nice=x\n", "nice=\n", "io_class=foo\n", "io_priority=8\n",
            "io_priority=-1\n", "sched_policy=deadline\n", "sched_policy=fifo\nsched_priority=0\n",
            "sched_policy=fifo\nsched_priority=100\n", "sched_policy=batch\nsched_priority=5\n",
            "sched_priority=5\n",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            if (load(bad[i], &s) != -1) { fprintf(stderr, "accepted: %s", bad[i]); return 1; }
        }
    }

    /* applied in the child; field 19 of /proc/self/stat is nice, 41 policy */
    const char *nice_probe = "cut -d\" \" -f19 /proc/self/stat";
    const char *pol_probe = "cut -d\" \" -f41 /proc/self/stat";
    assert(spawn_probe("nice=5\n", nice_probe, got, sizeof got) == 0 && !strcmp(got, "5"));
    assert(spawn_probe("priority=critical\nnice=3\n", nice_probe, got, sizeof got) == 0 && !strcmp(got, "3"));
    assert(spawn_probe("sched_policy=batch\n", pol_probe, got, sizeof got) == 0 && !strcmp(got, "3"));
    assert(spawn_probe("io_class=idle\n", "ionice", got, sizeof got) == 0 && !strcmp(got, "idle"));
    assert(spawn_probe("io_priority=6\n", "ionice", got, sizeof got) == 0 && !strcmp(got, "best-effort: prio 6"));
    if (geteuid() != 0) {
        /* what the rlimits don't grant fails the spawn; it doesn't run unscheduled */
        struct rlimit rn, rr;
        getrlimit(RLIMIT_NICE, &rn);
        getrlimit(RLIMIT_RTPRIO, &rr);
        if (rn.rlim_cur < 40)
            assert(spawn_probe("nice=-20\n", nice_probe, got, sizeof got) == 126 && !got[0]);
        if (rr.rlim_cur == 0)
            assert(spawn_probe("sched_policy=fifo\n", pol_probe, got, sizeof got) == 126 && !got[0]);
        else
            assert(spawn_probe("sched_policy=fifo\n", pol_probe, got, sizeof got) == 0 && !strcmp(got, "1"));
    } else {
        assert(spawn_probe("nice=-5\n", nice_probe, got, sizeof got) == 0 && !strcmp(got, "-5"));
        assert(spawn_probe("sched_policy=fifo\nsched_priority=10\n",
                           "cut -d\" \" -f40,41 /proc/self/stat", got, sizeof got) == 0 && !strcmp(got, "10 1"));
    }
    unlink(path);
    char of[320];
    snprintf(of, sizeof of, "%s/out", dir);
    unlink(of);
    rmdir(dir);
    printf("all sched tests passed\n");
    return 0;
}
