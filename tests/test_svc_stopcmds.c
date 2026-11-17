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

    /* umask= reaches exec_pre, exec and the stop runner; files the host would make 0644 */
    {
        char f1[320], f2[320], f3[320];
        struct stat st;
        int ws;
        umask(022);
        snprintf(f1, sizeof f1, "%s/pre", dir);
        snprintf(f2, sizeof f2, "%s/main", dir);
        snprintf(f3, sizeof f3, "%s/stop", dir);
        snprintf(body, sizeof body,
                 "name=u\nexec=/bin/touch\nargs=%s\noneshot=1\numask=0077\n"
                 "exec_pre=/bin/touch %s\nexec_stop=/bin/touch %s\n", f2, f1, f3);
        write_svc(p, body);
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(p, &svc) == 0 && svc.umask_val == 077);
        assert(service_spawn(&svc) == 0);
        assert(waitpid(svc.child_pid, &ws, 0) == svc.child_pid && WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
        assert(runner(&svc, 1) == 0);
        assert(stat(f1, &st) == 0 && (st.st_mode & 0777) == 0600);
        assert(stat(f2, &st) == 0 && (st.st_mode & 0777) == 0600);
        assert(stat(f3, &st) == 0 && (st.st_mode & 0777) == 0600);
        unlink(f1); unlink(f2); unlink(f3);
        /* a card is the service's own command too */
        snprintf(body, sizeof body, "name=u\nexec=/bin/true\numask=0077\ncard=exec:/bin/touch %s\n", f1);
        write_svc(p, body);
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(p, &svc) == 0 && svc.card_count == 1);
        pid_t cp = service_card_spawn(&svc, 0);
        assert(cp > 0 && waitpid(cp, &ws, 0) == cp && WIFEXITED(ws));
        assert(stat(f1, &st) == 0 && (st.st_mode & 0777) == 0600);
        unlink(f1);
        /* unset: the supervisor's own */
        snprintf(body, sizeof body, "name=u\nexec=/bin/touch\nargs=%s\noneshot=1\n", f2);
        write_svc(p, body);
        memset(&svc, 0, sizeof svc);
        assert(service_load_one(p, &svc) == 0);
        assert(service_spawn(&svc) == 0);
        assert(waitpid(svc.child_pid, &ws, 0) == svc.child_pid && WIFEXITED(ws));
        assert(stat(f2, &st) == 0 && (st.st_mode & 0777) == 0644);
        unlink(f2);
    }

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

    /* phase 3: a failing exec_stop still lets exec_stop_post run (#433) */
    unlink(out);
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\nexec_stop=/bin/false\n"
             "exec_stop_post=/bin/sh -c \"echo post >> %s\"\n", out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 3) != 0);
    slurp(out, buf, sizeof buf);
    assert(strcmp(buf, "post\n") == 0);

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

    /* working_dir=: commands run in it; a missing dir fails unless prefixed - */
    snprintf(body, sizeof body,
             "name=s\nexec=/bin/true\nworking_dir=%s\n"
             "exec_stop=/bin/sh -c \"pwd > %s\"\n", dir, out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 1) == 0);
    slurp(out, buf, sizeof buf);
    char want[300];
    snprintf(want, sizeof want, "%s\n", dir);
    assert(strcmp(buf, want) == 0);
    /* a missing dir never blocks stop commands: they run in / */
    snprintf(body, sizeof body, "name=s\nexec=/bin/true\nworking_dir=%s/nope\n"
             "exec_stop_post=/bin/sh -c \"pwd > %s\"\n", dir, out);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 2) == 0);
    slurp(out, buf, sizeof buf);
    assert(strcmp(buf, "/\n") == 0);
    snprintf(body, sizeof body, "name=s\nexec=/bin/true\nworking_dir=-%s/nope\nexec_stop=/bin/true\n", dir);
    write_svc(p, body);
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0);
    assert(runner(&svc, 1) == 0);
    write_svc(p, "name=s\nexec=/bin/true\nworking_dir=relative/dir\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == -1);
    write_svc(p, "name=s\nexec=/bin/true\nworking_dir=-~\n");
    memset(&svc, 0, sizeof svc);
    assert(service_load_one(p, &svc) == 0 && strcmp(svc.working_dir, "-~") == 0);

    unlink(p); unlink(out); rmdir(dir);
    printf("test_svc_stopcmds: PASS\n");
    return 0;
}
