#include "../reexec_state.h"
#include <assert.h>

static reexec_svc_t out[MAX_SERVICES];

static FILE *blob(const char *text) {
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    assert(f);
    return f;
}

static int parse_text(const char *text, reexec_global_t *g, int *nsvc, eviction_t *ev, int *nev,
                      char *err, size_t errsz) {
    FILE *f = blob(text);
    int rc = state_parse(f, g, out, MAX_SERVICES, nsvc, ev, nev, err, errsz);
    fclose(f);
    return rc;
}

int main(void) {
    static service_t svcs[3];
    reexec_global_t g, pg;
    eviction_t ev[2], pev[MAX_EVICTIONS];
    int nsvc, nev;
    char err[256];
    char *buf = NULL;
    size_t blen = 0;

    memset(svcs, 0, sizeof svcs);
    memset(&g, 0, sizeof g);
    memset(ev, 0, sizeof ev);

    strcpy(g.version, "0.4.0-1.973.git abc");
    strcpy(g.argv0, "/sbin/schema-init");
    g.init_start.tv_sec = 12; g.init_start.tv_nsec = 345678901;
    g.under_pressure = 1; g.last_stall_ms = 98765; g.last_reclaim_ms = 4321; g.nofile_soft = 1024;
    g.fd_ctl = 3; g.fd_notify = 4; g.fd_watchdog = -1; g.fd_client = 6; g.fd_oldexe = 7;

    strcpy(svcs[0].name, "dbus");
    svcs[0].content_hash = 4000000000u;
    svcs[0].child_pid = 199;
    svcs[0].inst.state = 4; svcs[0].inst.prev_state = 3; svcs[0].inst.weight = 8;
    svcs[0].inst.target_c = 4; svcs[0].inst.pid = 199; svcs[0].inst.flags = 0xdeadbeef;
    svcs[0].restart_count = 1;
    svcs[0].stable_time.tv_sec = 5; svcs[0].stable_time.tv_nsec = 7;
    strcpy(svcs[0].notify_status, "100% ready = yes\tok\n");
    strcpy(svcs[0].cgroup_path, "/sys/fs/cgroup/schema-init/dbus");
    svcs[0].is_frozen = 1;
    svcs[0].dormant_count = 200;
    svcs[0].flags = SVC_NO_RESTART;               /* held down by schema-ctl stop */
    svcs[0].listen_open = 2; svcs[0].listen_hold = 1;
    svcs[0].listen_fd[0] = 9; svcs[0].listen_fd[1] = 12; svcs[0].listen_fd[2] = -1; svcs[0].listen_fd[3] = 0;

    strcpy(svcs[1].name, "boot-timing");
    svcs[1].flags = 0;                        /* run-once timer, completed */
    svcs[1].timer_next.tv_sec = 360;
    svcs[1].last_start = -5;

    strcpy(svcs[2].name, "tick");
    svcs[2].flags = SVC_TIMER;
    svcs[2].timer_next.tv_sec = 1759300000; svcs[2].timer_next.tv_nsec = 999999999;
    svcs[2].ready_stale_dev = 66306; svcs[2].ready_stale_ino = UINT64_MAX;

    ev[0].pid = 4411; ev[0].deadline = 1759284000;
    strcpy(ev[0].cgroup, "/sys/fs/cgroup/schema-init/old one");

    FILE *w = open_memstream(&buf, &blen);
    state_write(w, &g, svcs, 3, ev, 1);
    fclose(w);

    /* round trip */
    assert(parse_text(buf, &pg, &nsvc, pev, &nev, err, sizeof err) == 0);
    assert(strcmp(pg.version, g.version) == 0);
    assert(strcmp(pg.argv0, g.argv0) == 0);
    assert(pg.init_start.tv_sec == 12 && pg.init_start.tv_nsec == 345678901);
    assert(pg.under_pressure == 1 && pg.last_stall_ms == 98765 && pg.last_reclaim_ms == 4321);
    assert(pg.nofile_soft == 1024);
    assert(out[0].rt.flags & SVC_NO_RESTART);
    assert(!(out[2].rt.flags & SVC_NO_RESTART));
    assert(pg.fd_ctl == 3 && pg.fd_notify == 4 && pg.fd_watchdog == -1 && pg.fd_client == 6 && pg.fd_oldexe == 7);
    assert(nsvc == 3 && nev == 1);
    assert(strcmp(out[0].name, "dbus") == 0 && out[0].hash == 4000000000u && out[0].timer == 0);
    assert(out[0].rt.child_pid == 199 && out[0].rt.restart_count == 1);
    assert(out[0].rt.inst.state == 4 && out[0].rt.inst.prev_state == 3 && out[0].rt.inst.weight == 8);
    assert(out[0].rt.inst.target_c == 4 && out[0].rt.inst.pid == 199 && out[0].rt.inst.flags == 0xdeadbeef);
    assert(out[0].rt.stable_time.tv_sec == 5 && out[0].rt.stable_time.tv_nsec == 7);
    assert(strcmp(out[0].rt.notify_status, "100% ready = yes\tok\n") == 0);
    assert(strcmp(out[0].rt.cgroup_path, "/sys/fs/cgroup/schema-init/dbus") == 0);
    assert(out[0].rt.is_frozen == 1 && out[0].rt.dormant_count == 200);
    assert(out[0].rt.listen_open == 2 && out[0].rt.listen_hold == 1);
    assert(out[0].rt.listen_fd[0] == 9 && out[0].rt.listen_fd[1] == 12 && out[0].rt.listen_fd[2] == -1);
    assert(out[1].rt.listen_open == 0);
    assert(strcmp(out[1].name, "boot-timing") == 0 && out[1].timer == 0);
    assert(out[1].rt.timer_next.tv_sec == 360 && out[1].rt.last_start == -5);
    assert(out[2].timer == 1);
    assert(out[2].rt.timer_next.tv_sec == 1759300000 && out[2].rt.timer_next.tv_nsec == 999999999);
    assert(out[2].rt.ready_stale_dev == 66306 && out[2].rt.ready_stale_ino == UINT64_MAX);
    assert(pev[0].pid == 4411 && pev[0].deadline == 1759284000);
    assert(strcmp(pev[0].cgroup, "/sys/fs/cgroup/schema-init/old one") == 0);

    /* a single escaped line: no raw space, newline or '=' leaks into a value */
    assert(strstr(buf, "notify_status=100%25%20ready%20%3D%20yes%09ok%0A "));

    /* every runtime field is written */
#define X(kind, f) assert(strstr(buf, " " #f "="));
    SVC_RUNTIME_FIELDS(X)
#undef X

    /* truncated: no end line */
    {
        char *t = strdup(buf);
        *strstr(t, "end\n") = '\0';
        assert(parse_text(t, &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
        assert(strstr(err, "truncated"));
        free(t);
    }

    /* unknown record and unknown key ignored; missing keys default */
    assert(parse_text("schema-init-state 1\n"
                      "future thing=1\n"
                      "svc name=a shiny=7 child_pid=5\n"
                      "end\n", &pg, &nsvc, pev, &nev, err, sizeof err) == 0);
    assert(nsvc == 1 && out[0].rt.child_pid == 5 && out[0].rt.restart_count == 0);
    assert(pg.fd_ctl == -1 && pg.fd_watchdog == -1);

    /* format gate */
    assert(parse_text("schema-init-state 2\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(strstr(err, "format 2"));
    assert(parse_text("schema-init-state 0\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("garbage\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);

    /* bad values */
    assert(parse_text("schema-init-state 1\nsvc name=a child_pid=x\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a dormant_count=256\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a timer_next=5.1\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a ready_stale_ino=-1\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a%ZZ\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a%4\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc child_pid=3\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a inst=1,2,3\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a listen_fd=3,4,5\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a listen_fd=3,4,5,6,7\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a listen_fd=3,-2,5,6\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nsvc name=a listen_fd=3,,5,6\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nfd ctl=-2\nend\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    assert(parse_text("schema-init-state 1\nend\nsvc name=a\n", &pg, &nsvc, pev, &nev, err, sizeof err) == -1);

    /* overflowing string field */
    {
        char t[1024] = "schema-init-state 1\nsvc name=";
        for (int i = 0; i < 70; i++) strcat(t, "a");
        strcat(t, "\nend\n");
        assert(parse_text(t, &pg, &nsvc, pev, &nev, err, sizeof err) == -1);
    }

    /* capacity */
    {
        FILE *f = blob("schema-init-state 1\nsvc name=a\nsvc name=b\nend\n");
        assert(state_parse(f, &pg, out, 1, &nsvc, pev, &nev, err, sizeof err) == -1);
        fclose(f);
    }

    /* reexec_compare */
    {
        static service_t ld[2];
        static reexec_svc_t b[3];
        memset(ld, 0, sizeof ld);
        memset(b, 0, sizeof b);
        strcpy(ld[0].name, "a"); ld[0].content_hash = 11;
        strcpy(ld[1].name, "new"); ld[1].content_hash = 22;
        strcpy(b[0].name, "a"); b[0].hash = 11; b[0].rt.child_pid = 50;
        strcpy(b[1].name, "gone"); b[1].hash = 33;            /* stopped, no .svc: fine */
        assert(reexec_compare(b, 2, ld, 2, err, sizeof err) == 0);

        b[1].rt.child_pid = 77;                               /* running, no .svc: orphan */
        assert(reexec_compare(b, 2, ld, 2, err, sizeof err) == -1 && strstr(err, "reload --evict"));
        b[1].rt.child_pid = 0; b[1].rt.failsafe_pid = 78;
        assert(reexec_compare(b, 2, ld, 2, err, sizeof err) == -1 && strstr(err, "pid 78"));
        b[1].rt.failsafe_pid = 0;

        ld[0].content_hash = 12;                              /* edited since boot */
        assert(reexec_compare(b, 2, ld, 2, err, sizeof err) == -1 && strstr(err, "modified since boot"));
        ld[0].content_hash = 0;                               /* unhashed parse: reload's exemption */
        assert(reexec_compare(b, 2, ld, 2, err, sizeof err) == 0);
        ld[0].content_hash = 11;

        strcpy(b[2].name, "a");                               /* duplicate */
        assert(reexec_compare(b, 3, ld, 2, err, sizeof err) == -1 && strstr(err, "twice"));
    }

    free(buf);
    printf("test_reexec_state: OK\n");
    return 0;
}
