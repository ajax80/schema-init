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

int main(void) {
    char dir[] = "/tmp/schema-killmode-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/k.svc", dir);
    service_t s;
    assert(load("name=k\nexec=/bin/true\n", &s) == 0 && s.kill_mode == KILL_MODE_DEFAULT &&
           s.kill_signal == 0 && s.final_kill_signal == 0);
    assert(load("name=k\nexec=/bin/true\nkill_mode=process\n", &s) == 0 && s.kill_mode == KILL_MODE_PROCESS);
    assert(load("name=k\nexec=/bin/true\nkill_mode=mixed\n", &s) == 0 && s.kill_mode == KILL_MODE_MIXED);
    assert(load("name=k\nexec=/bin/true\nkill_mode=control-group\n", &s) == 0 && s.kill_mode == KILL_MODE_CGROUP);
    assert(load("name=k\nexec=/bin/true\nkill_mode=cgroup\n", &s) == 0 && s.kill_mode == KILL_MODE_CGROUP);
    assert(load("name=k\nexec=/bin/true\nkill_mode=none\n", &s) == -1);
    assert(load("name=k\nexec=/bin/true\nkill_signal=SIGINT\nfinal_kill_signal=QUIT\n", &s) == 0 &&
           s.kill_signal == SIGINT && s.final_kill_signal == SIGQUIT);
    assert(load("name=k\nexec=/bin/true\nkill_signal=KILL\n", &s) == 0 && s.kill_signal == SIGKILL);
    assert(load("name=k\nexec=/bin/true\nkill_signal=2\n", &s) == 0 && s.kill_signal == 2);
    assert(load("name=k\nexec=/bin/true\nkill_signal=STOP\n", &s) == -1);
    assert(load("name=k\nexec=/bin/true\nkill_signal=19\n", &s) == -1);
    assert(load("name=k\nexec=/bin/true\nfinal_kill_signal=nope\n", &s) == -1);
    {
        char dd[400], dp[450];
        snprintf(dd, sizeof dd, "%s.d", path);
        assert(mkdir(dd, 0755) == 0);
        snprintf(dp, sizeof dp, "%s/10.conf", dd);
        FILE *f = fopen(dp, "w");
        fputs("kill_mode=\nkill_signal=\nfinal_kill_signal=\n", f);
        fclose(f);
        assert(load("name=k\nexec=/bin/true\nkill_mode=process\nkill_signal=INT\nfinal_kill_signal=QUIT\n", &s) == 0);
        assert(s.kill_mode == KILL_MODE_DEFAULT && s.kill_signal == 0 && s.final_kill_signal == 0);
        unlink(dp);
        rmdir(dd);
    }
    unlink(path);
    rmdir(dir);
    printf("test_svc_kill_mode: ok\n");
    return 0;
}
