#include "../sdbus_activate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* write a .service file into dir */
static void put(const char *dir, const char *fn, const char *body) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, fn);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}

static void test_parse(void) {
    char dir[] = "/tmp/sdbus-act-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "real.service",
        "[D-BUS Service]\nName=com.example.Real\nExec=/usr/libexec/realsvc --arg\nUser=root\n");
    put(dir, "nouser.service",
        "[D-BUS Service]\nName=com.example.NoUser\nExec=/usr/libexec/nu\n");
    put(dir, "false.service",
        "[D-BUS Service]\nName=com.example.Systemd\nExec=/bin/false\nSystemdService=x.service\n");
    put(dir, "noname.service", "[D-BUS Service]\nExec=/usr/libexec/x\n");

    sdbus_svctab *t = sdbus_svctab_parse_dir(dir);
    assert(t);

    const sdbus_svc_ent *r = sdbus_svctab_find(t, "com.example.Real");
    assert(r);
    assert(!strcmp(r->argv[0], "/usr/libexec/realsvc"));
    assert(!strcmp(r->argv[1], "--arg"));
    assert(r->argv[2] == NULL);
    assert(!strcmp(r->user, "root"));

    const sdbus_svc_ent *n = sdbus_svctab_find(t, "com.example.NoUser");
    assert(n && !strcmp(n->user, "root"));           /* default */

    assert(sdbus_svctab_find(t, "com.example.Systemd") == NULL);   /* /bin/false skipped */
    assert(sdbus_svctab_find(t, "com.example.Missing") == NULL);   /* absent */
    assert(t->n == 2);                                /* real + nouser only */

    sdbus_svctab_free(t);
    /* cleanup */
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_parse OK\n");
}

static void test_masklist(void) {
    char dir[] = "/tmp/sdbus-mask-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "keep.service",
        "[D-BUS Service]\nName=com.example.Keep\nExec=/usr/libexec/keep\nUser=root\n");
    put(dir, "polkit.service",
        "[D-BUS Service]\nName=org.freedesktop.PolicyKit1\nExec=/usr/lib/polkit-1/polkitd --no-debug\nUser=root\n");

    /* no maskfile → back-compat, both activatable */
    sdbus_svctab *t0 = sdbus_svctab_parse_dir(dir);
    assert(sdbus_svctab_find(t0, "com.example.Keep"));
    assert(sdbus_svctab_find(t0, "org.freedesktop.PolicyKit1"));
    sdbus_svctab_free(t0);

    /* maskfile masks PolicyKit1 (comment + blank + surrounding whitespace tolerated) */
    char mf[512]; snprintf(mf, sizeof mf, "%s/masked", dir);
    FILE *f = fopen(mf, "w"); assert(f);
    fputs("# schema-managed daemons, never bus-activate\n\n  org.freedesktop.PolicyKit1  \n", f);
    fclose(f);

    sdbus_svctab *t = sdbus_svctab_parse_dir_masked(dir, mf);
    assert(sdbus_svctab_find(t, "com.example.Keep"));                     /* unaffected */
    assert(sdbus_svctab_find(t, "org.freedesktop.PolicyKit1") == NULL);   /* masked */
    assert(t->n == 1);
    sdbus_svctab_free(t);

    /* absent maskfile path → no masking */
    sdbus_svctab *t2 = sdbus_svctab_parse_dir_masked(dir, "/nonexistent/masked");
    assert(sdbus_svctab_find(t2, "org.freedesktop.PolicyKit1"));
    sdbus_svctab_free(t2);

    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_masklist OK\n");
}

static sdbus_held_msg mk(int caller, uint32_t serial, sdbus_held_kind k) {
    sdbus_held_msg m = {0};
    m.bytes = (unsigned char *)strdup("wire"); m.len = 4;
    m.caller_id = caller; m.serial = serial; m.expects_reply = 1; m.kind = k;
    return m;
}

static void test_pending(void) {
    sdbus_acts *a = sdbus_acts_new();
    assert(sdbus_acts_find(a, "com.x") == NULL);
    assert(sdbus_acts_next_deadline(a) == -1);

    sdbus_pending_act *e = sdbus_acts_begin(a, "com.x", 4242, 1000);
    assert(e && e->child_pid == 4242 && e->n_held == 0);
    assert(sdbus_acts_find(a, "com.x") == e);
    assert(sdbus_acts_by_pid(a, 4242) == e);
    assert(sdbus_acts_next_deadline(a) == 1000);

    sdbus_held_msg m1 = mk(10, 1, SDBUS_HELD_IMPLICIT);
    sdbus_held_msg m2 = mk(11, 2, SDBUS_HELD_EXPLICIT);
    sdbus_acts_hold(e, &m1); sdbus_acts_hold(e, &m2);
    free(m1.bytes); free(m2.bytes);                 /* hold deep-copies */
    assert(sdbus_acts_find(a, "com.x")->n_held == 2);

    /* take removes the entry and hands back the held array */
    sdbus_held_msg *out = NULL; int n = 0;
    assert(sdbus_acts_take(a, "com.x", &out, &n) == 1);
    assert(n == 2 && out[0].caller_id == 10 && out[1].kind == SDBUS_HELD_EXPLICIT);
    assert(sdbus_acts_find(a, "com.x") == NULL);     /* gone */
    for (int i = 0; i < n; i++) { free(out[i].bytes); free(out[i].fds); }
    free(out);

    /* reap_expired picks the entry whose deadline has passed */
    sdbus_acts_begin(a, "com.y", 5, 500);
    sdbus_acts_begin(a, "com.z", 6, 3000);
    sdbus_held_msg *o2 = NULL; int n2 = 0;
    assert(sdbus_acts_reap_expired(a, 600, &o2, &n2) == 1);   /* com.y expired */
    assert(sdbus_acts_find(a, "com.y") == NULL);
    assert(sdbus_acts_find(a, "com.z") != NULL);              /* not yet */
    free(o2);
    assert(sdbus_acts_reap_expired(a, 600, &o2, &n2) == 0);   /* none left expired */

    sdbus_acts_free(a);
    printf("test_pending OK\n");
}

static void test_should_drop_privs(void) {
    assert(sdbus_activate_should_drop_privs(1, 1000) == 1);   /* system bus, non-root target: drop */
    assert(sdbus_activate_should_drop_privs(1, 0) == 0);      /* system bus, root target: no-op today, unchanged */
    assert(sdbus_activate_should_drop_privs(0, 1000) == 0);   /* session bus: never drop */
    assert(sdbus_activate_should_drop_privs(0, 0) == 0);      /* session bus: never drop, even if User=root */
    printf("test_should_drop_privs OK\n");
}

static int env_has(char **env, const char *entry) {
    for (char **p = env; p && *p; p++) if (!strcmp(*p, entry)) return 1;
    return 0;
}

static void test_build_env(void) {
    /* system bus: exact 3-entry clean env, unchanged from today's literal array */
    char **sys_env = sdbus_activate_build_env(1, "unix:path=/run/dbus/system_bus_socket", NULL);
    int n = 0; for (char **p = sys_env; p && *p; p++) n++;
    assert(n == 3);
    assert(env_has(sys_env, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin"));
    assert(env_has(sys_env, "DBUS_STARTER_ADDRESS=unix:path=/run/dbus/system_bus_socket"));
    assert(env_has(sys_env, "DBUS_STARTER_BUS_TYPE=system"));
    sdbus_activate_free_env(sys_env);

    /* session bus: DBUS_STARTER_BUS_TYPE=session, plus every inherited var passed through */
    char *fake_inherited[] = {
        (char *)"WAYLAND_DISPLAY=wayland-0",
        (char *)"XDG_RUNTIME_DIR=/run/user/1000",
        (char *)"HOME=/home/ajax80",
        NULL
    };
    char **sess_env = sdbus_activate_build_env(0, "unix:path=/run/user/1000/bus", fake_inherited);
    n = 0; for (char **p = sess_env; p && *p; p++) n++;
    assert(n == 5);   /* DBUS_STARTER_ADDRESS + DBUS_STARTER_BUS_TYPE + 3 inherited */
    assert(env_has(sess_env, "DBUS_STARTER_ADDRESS=unix:path=/run/user/1000/bus"));
    assert(env_has(sess_env, "DBUS_STARTER_BUS_TYPE=session"));
    assert(env_has(sess_env, "WAYLAND_DISPLAY=wayland-0"));
    assert(env_has(sess_env, "XDG_RUNTIME_DIR=/run/user/1000"));
    assert(env_has(sess_env, "HOME=/home/ajax80"));
    sdbus_activate_free_env(sess_env);

    /* session bus with no inherited vars at all -- degrades to just the 2 DBUS_STARTER_* */
    char **empty_env = sdbus_activate_build_env(0, "unix:path=/x", NULL);
    n = 0; for (char **p = empty_env; p && *p; p++) n++;
    assert(n == 2);
    sdbus_activate_free_env(empty_env);

    printf("test_build_env OK\n");
}

static void test_default_user_session_mode(void) {
    char dir[] = "/tmp/sdbus-defuser-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "nouser.service",
        "[D-BUS Service]\nName=com.example.NoUser\nExec=/usr/libexec/nu\n");
    put(dir, "hasuser.service",
        "[D-BUS Service]\nName=com.example.HasUser\nExec=/usr/libexec/hu\nUser=nobody\n");

    const char *dirs[] = { dir };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 1, NULL, "ajax80");
    assert(t);

    const sdbus_svc_ent *n = sdbus_svctab_find(t, "com.example.NoUser");
    assert(n && !strcmp(n->user, "ajax80"));           /* absent User= -> the passed default */

    const sdbus_svc_ent *h = sdbus_svctab_find(t, "com.example.HasUser");
    assert(h && !strcmp(h->user, "nobody"));           /* explicit User= always wins */

    sdbus_svctab_free(t);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_default_user_session_mode OK\n");
}

static void test_multidir_override_precedence(void) {
    char dir1[] = "/tmp/sdbus-multidir1-XXXXXX";
    char dir2[] = "/tmp/sdbus-multidir2-XXXXXX";
    assert(mkdtemp(dir1));
    assert(mkdtemp(dir2));

    /* same Name in both dirs, different Exec -- dir1 (listed first) must win */
    put(dir1, "shared.service",
        "[D-BUS Service]\nName=com.example.Shared\nExec=/from/dir1\n");
    put(dir2, "shared.service",
        "[D-BUS Service]\nName=com.example.Shared\nExec=/from/dir2\n");
    /* dir2-only entry must still show up */
    put(dir2, "onlydir2.service",
        "[D-BUS Service]\nName=com.example.OnlyDir2\nExec=/from/dir2/only\n");

    const char *dirs[] = { dir1, dir2 };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    assert(t->n == 2);

    const sdbus_svc_ent *s = sdbus_svctab_find(t, "com.example.Shared");
    assert(s && !strcmp(s->argv[0], "/from/dir1"));    /* dir1 shadowed dir2 */

    const sdbus_svc_ent *o = sdbus_svctab_find(t, "com.example.OnlyDir2");
    assert(o && !strcmp(o->argv[0], "/from/dir2/only"));

    sdbus_svctab_free(t);
    char cmd1[600]; snprintf(cmd1, sizeof cmd1, "rm -rf %s", dir1); (void)system(cmd1);
    char cmd2[600]; snprintf(cmd2, sizeof cmd2, "rm -rf %s", dir2); (void)system(cmd2);
    printf("test_multidir_override_precedence OK\n");
}

static void test_multidir_missing_dir_skipped(void) {
    char dir[] = "/tmp/sdbus-multidir-missing-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "real.service", "[D-BUS Service]\nName=com.example.Real\nExec=/x\n");

    const char *dirs[] = { "/nonexistent/does/not/exist", dir };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    assert(t->n == 1);
    assert(sdbus_svctab_find(t, "com.example.Real"));

    sdbus_svctab_free(t);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_multidir_missing_dir_skipped OK\n");
}

int main(void) {
    test_parse();
    test_masklist();
    test_pending();
    test_should_drop_privs();
    test_build_env();
    test_default_user_session_mode();
    test_multidir_override_precedence();
    test_multidir_missing_dir_skipped();
    /* UpdateActivationEnvironment helper: replace in place, append new, reject bad keys */
    {
        char *base[] = { "HOME=/h", "PATH=/a", NULL };
        char **env = sdbus_activate_env_dup(base);
        assert(sdbus_activate_env_set(&env, "PATH", "/b") == 0);
        assert(sdbus_activate_env_set(&env, "WAYLAND_DISPLAY", "wayland-0") == 0);
        assert(sdbus_activate_env_set(&env, "PAT", "x") == 0);
        assert(sdbus_activate_env_set(&env, "", "x") == -1);
        assert(sdbus_activate_env_set(&env, "A=B", "x") == -1);
        assert(!strcmp(env[0], "HOME=/h") && !strcmp(env[1], "PATH=/b"));
        assert(!strcmp(env[2], "WAYLAND_DISPLAY=wayland-0") && !strcmp(env[3], "PAT=x") && !env[4]);
        char **built = sdbus_activate_build_env(0, "unix:path=/x", env);
        int saw = 0;
        for (char **p = built; *p; p++) if (!strcmp(*p, "WAYLAND_DISPLAY=wayland-0")) saw = 1;
        assert(saw);
        sdbus_activate_free_env(built);
        sdbus_activate_free_env(env);
    }
    printf("all sdbus_activate tests passed\n");
    return 0;
}
