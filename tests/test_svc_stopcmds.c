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

static int runner(service_t *svc, int phase) {
    int st;
    pid_t p = service_stop_spawn(svc, phase);
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
    char dir[] = "/tmp/schema-stopcmds-XXXXXX", p[300], out[300], body[2048], buf[256];
    service_t svc;
    assert(mkdtemp(dir));
    snprintf(p, sizeof p, "%s/s.svc", dir);
    snprintf(out, sizeof out, "%s/out", dir);

    /* parse: two exec_stop, one ignore-failure exec_stop_post */
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\n"
             "exec_stop=/bin/sh -c \"echo one >> %s\"\n"
             "exec_stop=/bin/sh -c \"echo two >> %s\"\n"
             "exec_stop_post=-/bin/false\n", out, out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(svc.exec_stop_count == 2 && svc.exec_stop_post_count == 1);
    assert(svc.exec_stop_post[0][0] == '-');

    /* the runner: commands in order, exit 0 */
    assert(runner(&svc, 1) == 0);
    slurp(out, buf, sizeof buf);
    assert(strcmp(buf, "one\ntwo\n") == 0);
    /* an ignored failure still exits 0 */
    assert(runner(&svc, 2) == 0);

    /* a plain failure stops the chain and the runner exits non-zero */
    unlink(out);
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\nexec_stop=/bin/false\n"
             "exec_stop=/bin/sh -c \"echo never >> %s\"\n", out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 1) != 0);
    slurp(out, buf, sizeof buf);
    assert(buf[0] == '\0');

    /* phase 3 (eviction): stop then stop_post, back to back */
    unlink(out);
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\nexec_stop=/bin/sh -c \"echo stop >> %s\"\n"
             "exec_stop_post=/bin/sh -c \"echo post >> %s\"\n", out, out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 3) == 0);
    slurp(out, buf, sizeof buf);
    assert(strcmp(buf, "stop\npost\n") == 0);

    /* an empty value in a drop-in clears the list */
    char dd[320], dp[340];
    write_svc(p, "name=s\nexec=/bin/true\nexec_stop=/bin/true\n");
    snprintf(dd, sizeof dd, "%s.d", p);
    mkdir(dd, 0755);
    snprintf(dp, sizeof dp, "%s/10-clear.conf", dd);
    write_svc(dp, "exec_stop=\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0 && svc.exec_stop_count == 0);
    unlink(dp); rmdir(dd);

    /* nine of them: load refused, like exec_pre */
    body[0] = '\0';
    strcat(body, "name=s\nexec=/bin/true\n");
    for (int i = 0; i < 9; i++) strcat(body, "exec_stop=/bin/true\n");
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) != 0);

    unlink(p); unlink(out); rmdir(dir);
    printf("test_svc_stopcmds: PASS\n");
    return 0;
}
