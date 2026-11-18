#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* audit 4.10: supplementary_groups= at the uid drop. Root only: setgroups. */
static char dir[] = "/tmp/schema-suppg-XXXXXX";
static char path[300], out[320];

static int run(const char *keys, char *got, size_t n) {
    static service_t s;
    int st;
    FILE *f = fopen(path, "w");
    fprintf(f, "name=t\nexec=/bin/sh\nargs=-c\nargs=id -G > %s\noneshot=1\n%s", out, keys);
    fclose(f);
    unlink(out);
    memset(&s, 0, sizeof s);
    assert(service_load_one(path, &s) == 0);
    assert(service_spawn(&s) == 0);
    assert(waitpid(s.child_pid, &st, 0) == s.child_pid && WIFEXITED(st));
    got[0] = 0;
    if ((f = fopen(out, "r"))) {
        if (!fgets(got, (int)n, f)) got[0] = 0;
        fclose(f);
    }
    got[strcspn(got, "\n")] = 0;
    return WEXITSTATUS(st);
}

static int has(const char *list, const char *g) {
    char b[256];
    snprintf(b, sizeof b, " %s ", list);
    char w[32];
    snprintf(w, sizeof w, " %s ", g);
    return strstr(b, w) != NULL;
}

int main(void) {
    char got[256];
    if (geteuid() != 0) { printf("test_supp_groups: skipped (needs root)\n"); return 0; }
    assert(mkdtemp(dir));
    chmod(dir, 0777);
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(out, sizeof out, "%s/out", dir);

    /* added on top of the user's own groups, by name or gid */
    assert(run("user=nobody\nsupplementary_groups=adm 10\nno_new_privs=0\n", got, sizeof got) == 0);
    assert(has(got, "4") && has(got, "10") && has(got, "65534"));
    /* a root service gets them too */
    assert(run("supplementary_groups=adm\n", got, sizeof got) == 0 && has(got, "4") && has(got, "0"));
    /* duplicates collapse */
    assert(run("user=nobody\nsupplementary_groups=4 adm\n", got, sizeof got) == 0 && has(got, "4"));
    /* an unknown group fails the start rather than running without it */
    assert(run("user=nobody\nsupplementary_groups=no_such_group_x\n", got, sizeof got) == 126 && !got[0]);
    /* a numeric gid must exist too, like group= */
    assert(run("user=nobody\nsupplementary_groups=99999\n", got, sizeof got) == 126);
    /* without the key nothing extra appears */
    assert(run("user=nobody\n", got, sizeof got) == 0 && !has(got, "4"));

    /* a dynamic uid: no passwd entry, so no initgroups list (never nobody's);
     * only its own gid plus supplementary_groups, and systemd's env */
    {
        static service_t d;
        int st;
        FILE *f = fopen(path, "w");
        fprintf(f, "name=dyn\nexec=/bin/sh\nargs=-c\nargs=echo $(id -u) $(id -g) [$(id -G)] $USER $HOME > %s\n"
                   "oneshot=1\ndynamic_user=1\nsupplementary_groups=adm\nprivate_tmp=0\nprotect_system=0\n"
                   "protect_home=0\n", out);
        fclose(f);
        unlink(out);
        memset(&d, 0, sizeof d);
        assert(service_load_one(path, &d) == 0);
        d.dyn_uid = 64000;
        d.run_uid = d.run_gid = 64000;
        assert(service_spawn(&d) == 0);
        assert(waitpid(d.child_pid, &st, 0) == d.child_pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        f = fopen(out, "r");
        assert(f && fgets(got, sizeof got, f));
        fclose(f);
        got[strcspn(got, "\n")] = 0;
        if (strcmp(got, "64000 64000 [64000 4] dyn /")) { fprintf(stderr, "dynamic: %s\n", got); return 1; }
    }

    /* a dynamic uid keeps its ambient caps (earlyoom: CAP_KILL) */
    {
        static service_t d;
        int st;
        FILE *f = fopen(path, "w");
        fprintf(f, "name=dyn\nexec=/bin/sh\nargs=-c\nargs=grep CapAmb /proc/self/status > %s\n"
                   "oneshot=1\ndynamic_user=1\nambient_caps=CAP_KILL\nprivate_tmp=0\nprotect_system=0\n"
                   "protect_home=0\n", out);
        fclose(f);
        unlink(out);
        memset(&d, 0, sizeof d);
        assert(service_load_one(path, &d) == 0);
        d.dyn_uid = 64000;
        d.run_uid = d.run_gid = 64000;
        assert(service_spawn(&d) == 0);
        assert(waitpid(d.child_pid, &st, 0) == d.child_pid && WIFEXITED(st));
        if (WEXITSTATUS(st) != 0) { fprintf(stderr, "dynamic + ambient_caps exited %d\n", WEXITSTATUS(st)); return 1; }
        f = fopen(out, "r");
        assert(f && fgets(got, sizeof got, f));
        fclose(f);
        assert(strstr(got, "0000000000000020"));   /* CAP_KILL = bit 5 */
    }

    /* the stop runner drops the same way */
    {
        static service_t r;
        int st;
        char stop[320];
        snprintf(stop, sizeof stop, "%s/stop", dir);
        FILE *f = fopen(path, "w");
        fprintf(f, "name=t\nexec=/bin/true\nuser=nobody\nsupplementary_groups=adm\n"
                   "exec_stop=/bin/sh -c \"id -G > %s\"\n", stop);
        fclose(f);
        memset(&r, 0, sizeof r);
        assert(service_load_one(path, &r) == 0);
        pid_t p = service_stop_spawn(&r, 1);
        assert(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        f = fopen(stop, "r");
        assert(f && fgets(got, sizeof got, f));
        fclose(f);
        got[strcspn(got, "\n")] = 0;
        assert(has(got, "4") && has(got, "65534"));
        unlink(stop);
    }
    unlink(out);
    unlink(path);
    rmdir(dir);
    printf("all supp groups tests passed\n");
    return 0;
}
