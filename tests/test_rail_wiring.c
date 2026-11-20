/* service_log with and without the VT hook: stdout and rail.log keep today's
 * plain line unless the hook is set, and rail.log keeps it even then. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../schema.h"
#include "../service.h"

static char hooked_event[64], hooked_line[512];
static int hook_calls;

static void hook(const service_t *svc, const char *event, const char *line, size_t n) {
    (void)svc;
    snprintf(hooked_event, sizeof hooked_event, "%s", event);
    snprintf(hooked_line, sizeof hooked_line, "%.*s", (int)n, line);
    hook_calls++;
}

static size_t slurp(const char *path, char *b, size_t sz) {
    int fd = open(path, O_RDONLY);
    ssize_t n = fd < 0 ? 0 : read(fd, b, sz - 1);
    if (fd >= 0) close(fd);
    b[n > 0 ? n : 0] = 0;
    return n > 0 ? (size_t)n : 0;
}

/* the part after the timestamp, which is today's fixed format */
static const char *body(const char *l) { return strchr(l, ']') ? strchr(l, ']') + 2 : l; }

int main(void) {
    char dir[] = "/tmp/railwXXXXXX", out[256], log[256], b1[4096], b2[4096];
    static service_t s;
    assert(mkdtemp(dir));
    snprintf(out, sizeof out, "%s/stdout", dir);
    snprintf(log, sizeof log, "%s/rail.log", dir);
    service_log_dir = dir;
    snprintf(s.name, sizeof s.name, "sshd");
    s.inst.state = STATE_NEW_PROCESS;
    s.child_pid = 1300;

    fflush(stdout);
    int keep = dup(1), fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, 1);
    service_log(&s, "spawn");
    service_log_hook = hook;
    service_log(&s, "notify-ready");
    fflush(stdout);
    dup2(keep, 1);

    slurp(out, b1, sizeof b1);
    slurp(log, b2, sizeof b2);
    const char *want1 = "sshd                  spawn         state=NEW_PROCESS   wt=0  pid=1300\n";
    const char *want2 = "sshd                  notify-ready  state=NEW_PROCESS   wt=0  pid=1300\n";
    /* stdout: only the unhooked line */
    assert(strchr(b1, '\n') && !strchr(strchr(b1, '\n') + 1, '\n'));
    assert(!strcmp(body(b1), want1));
    /* rail.log: both, plain */
    char *second = strchr(b2, '\n') + 1;
    assert(!strncmp(body(b2), want1, strlen(want1)));
    assert(!strcmp(body(second), want2));
    /* the hook got the event and the same plain line */
    assert(hook_calls == 1 && !strcmp(hooked_event, "notify-ready") && !strcmp(body(hooked_line), want2));
    char rm[300];
    snprintf(rm, sizeof rm, "rm -rf %s", dir);
    (void)!system(rm);
    printf("test_rail_wiring: ok\n");
    return 0;
}
