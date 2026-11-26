#include "../service.h"
#include "../schema.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-order-XXXXXX";
static char path[300], dd[320], dp[360];
static service_t t[MAX_SERVICES];

static void put(const char *p, const char *body) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int load(const char *keys, const char *dropin, service_t *s) {
    char body[4096];
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    put(path, body);
    if (dropin) {
        mkdir(dd, 0755);
        put(dp, dropin);
    }
    memset(s, 0, sizeof *s);
    int r = service_load_one(path, s);
    if (dropin) {
        unlink(dp);
        rmdir(dd);
    }
    return r;
}

static void reset(int n) {
    memset(t, 0, sizeof t);
    for (int i = 0; i < n; i++) {
        snprintf(t[i].name, sizeof t[i].name, "s%d", i);
        for (int d = 0; d < MAX_DEPS; d++) t[i].dep_idx[d] = t[i].grp_dep_idx[d] = -1;
        t[i].inst.state = STATE_PERFECT;
    }
}

static void after(int from, int to) {
    int k = 0;
    while (t[from].after_name[k][0]) k++;
    snprintf(t[from].after_name[k], 64, "s%d", to);
}

static void before(int from, int to) {
    int k = 0;
    while (t[from].before_name[k][0]) k++;
    snprintf(t[from].before_name[k], 64, "s%d", to);
}

static int keep_t(const char *name) { return !strcmp(name, "t"); }
static int keep_m1(const char *name) { return !strcmp(name, "m@1"); }

static int load_file(const char *p, service_t *s) {
    memset(s, 0, sizeof *s);
    return service_load_one(p, s);
}

static void drop(service_t *s) {
    for (int k = 1; k < MAX_ARGV; k++) free(s->argv[k]);
    service_free_landlock(s);
}

static int has_edge(int s, int target) {
    for (int k = 0; k < t[s].order_count; k++)
        if (t[s].order_idx[k] == target) return 1;
    return 0;
}

int main(void) {
    static service_t s;
    char keys[4096];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(dd, sizeof dd, "%s.d", path);
    snprintf(dp, sizeof dp, "%s/10-x.conf", dd);

    /* parse: repeats collapse, before= is its own list, a 17th is refused */
    assert(load("after=a\nafter=b\nafter=a\nbefore=c\n", NULL, &s) == 0);
    assert(!strcmp(s.after_name[0], "a") && !strcmp(s.after_name[1], "b") && !s.after_name[2][0]);
    assert(!strcmp(s.before_name[0], "c") && !s.before_name[1][0]);
    keys[0] = '\0';
    for (int i = 0; i < MAX_ORDER; i++)
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "after=x%d\n", i);
    assert(load(keys, NULL, &s) == 0);
    strcat(keys, "after=one-too-many\n");
    assert(load(keys, NULL, &s) != 0);
    assert(load("after=\n", NULL, &s) != 0);
    /* one name per line; trailing blanks trimmed */
    assert(load("after=a b\n", NULL, &s) != 0);
    assert(load("before=a\tb\n", NULL, &s) != 0);
    assert(load("after=a  \t\n", NULL, &s) == 0 && !strcmp(s.after_name[0], "a"));
    /* an empty after= in a drop-in clears the base file's list */
    assert(load("after=a\nbefore=b\n", "after=\nafter=z\nbefore=\n", &s) == 0);
    assert(!strcmp(s.after_name[0], "z") && !s.after_name[1][0] && !s.before_name[0][0]);

    /* resolve: own after= plus another's before=, unique; unknown names ignored */
    reset(3); after(0, 1); after(0, 1); before(1, 0); before(2, 0);
    snprintf(t[0].after_name[2], 64, "missing");
    services_resolve_order(t, 3, NULL, 0);
    assert(t[0].order_count == 2 && has_edge(0, 1) && has_edge(0, 2));
    assert(!t[1].order_count && !t[2].order_count);

    /* a cycle of order edges keeps the first and drops the closing one */
    reset(2); after(0, 1); after(1, 0);
    services_resolve_order(t, 2, NULL, 0);
    assert(has_edge(0, 1) && !t[1].order_count);
    /* a cycle through a dep= drops the order edge, never the dep */
    reset(3); t[0].dep_idx[0] = 1; after(1, 2); after(2, 0);
    services_resolve_order(t, 3, NULL, 0);
    assert(has_edge(1, 2) && !t[2].order_count);
    /* through a dep= on a group: s0 dep=G{s1}, s1 after=s0 would hold both forever */
    {
        static group_t g[1];
        memset(g, 0, sizeof g);
        g[0].member_idx[0] = 1;
        g[0].member_count = 1;
        reset(2); snprintf(t[0].dep_name[0], 64, "G"); t[0].grp_dep_idx[0] = 0; after(1, 0);
        services_resolve_order(t, 2, g, 1);
        assert(!t[1].order_count);
        services_resolve_order(t, 2, NULL, 0);
        assert(t[1].order_count == 1);
    }
    reset(2); after(0, 0); before(1, 1);
    services_resolve_order(t, 2, NULL, 0);
    assert(!t[0].order_count && !t[1].order_count);
    /* re-resolving (a reload) starts over rather than appending */
    reset(2); after(0, 1);
    services_resolve_order(t, 2, NULL, 0);
    services_resolve_order(t, 2, NULL, 0);
    assert(t[0].order_count == 1);

    /* ready: queued or spawned-not-ready holds; anything settled either way lets go */
    reset(2); after(0, 1);
    services_resolve_order(t, 2, NULL, 0);
    static const struct { uint8_t st; int restarts, ready; } c[] = {
        { STATE_NEW_PROCESS, 0, 0 }, { STATE_FULL_TRUST, 0, 0 }, { STATE_FULL_TRUST, 2, 1 },
        { STATE_NEW_PROCESS, 1, 1 }, { STATE_FUNDAMENTAL, 0, 1 }, { STATE_SETTLED, 0, 1 },
        { STATE_PERFECT, 0, 1 }, { STATE_RECOVERY, 1, 1 }, { STATE_FRICTION, 3, 1 },
        { STATE_DORMANT, 3, 1 }, { STATE_EXCISED, 0, 1 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        t[1].inst.state = c[i].st;
        t[1].restart_count = c[i].restarts;
        assert(service_order_ready(&t[0], t, 2, NULL, 0) == c[i].ready);
    }
    reset(1);
    assert(service_order_ready(&t[0], t, 1, NULL, 0));

    /* a queued predecessor that can never start lets go: s0 after=s1, s1 dep=s2 */
    reset(3); after(0, 1);
    services_resolve_order(t, 3, NULL, 0);
    t[1].inst.state = STATE_NEW_PROCESS;
    snprintf(t[1].dep_name[0], 64, "s2"); t[1].dep_idx[0] = 2;
    t[2].inst.state = STATE_EXCISED;
    assert(service_order_ready(&t[0], t, 3, NULL, 0) == 0);     /* non-critical: s1 will start, wait */
    t[2].flags |= SVC_CRITICAL;
    assert(service_order_ready(&t[0], t, 3, NULL, 0) == 1);     /* critical excised: s1 never will */
    t[2].inst.state = STATE_DORMANT;
    assert(service_order_ready(&t[0], t, 3, NULL, 0) == 1);
    t[2].inst.state = STATE_FULL_TRUST;
    assert(service_order_ready(&t[0], t, 3, NULL, 0) == 0);     /* s2 still starting: wait */
    /* transitive: s2 itself queued behind a critical excised s3 */
    reset(4); after(0, 1);
    services_resolve_order(t, 4, NULL, 0);
    t[1].inst.state = t[2].inst.state = STATE_NEW_PROCESS;
    snprintf(t[1].dep_name[0], 64, "s2"); t[1].dep_idx[0] = 2;
    snprintf(t[2].dep_name[0], 64, "s3"); t[2].dep_idx[0] = 3;
    t[3].inst.state = STATE_FULL_TRUST;
    assert(service_order_ready(&t[0], t, 4, NULL, 0) == 0);
    t[3].inst.state = STATE_EXCISED; t[3].flags |= SVC_CRITICAL;
    assert(service_order_ready(&t[0], t, 4, NULL, 0) == 1);
    /* a wait_socket= that hasn't appeared, and an excised group dep */
    reset(2); after(0, 1);
    services_resolve_order(t, 2, NULL, 0);
    t[1].inst.state = STATE_NEW_PROCESS;
    assert(service_order_ready(&t[0], t, 2, NULL, 0) == 0);
    t[1].sock_waiting = 1;
    assert(service_order_ready(&t[0], t, 2, NULL, 0) == 1);
    t[1].sock_waiting = 0;
    {
        uint8_t gs[1] = { STATE_FULL_TRUST };
        snprintf(t[1].dep_name[0], 64, "G"); t[1].grp_dep_idx[0] = 0;
        assert(service_order_ready(&t[0], t, 2, gs, 1) == 0);
        gs[0] = STATE_EXCISED;
        assert(service_order_ready(&t[0], t, 2, gs, 1) == 1);
    }

    /* a relative exec= (not ordering, but this file has the loader harness): refused,
     * unless the engine says that service is already loaded (re-exec/reload keep it).
     * Asked once the name is final: name= after exec=, a drop-in's exec=, a name from
     * the file name, an instance's name */
    assert(load("exec=relative/path\n", NULL, &s) != 0);
    assert(load("exec=true\n", NULL, &s) != 0);
    assert(load("", "exec=bin/x\n", &s) != 0);
    assert(load("exec=x\n", "exec=/bin/x\n", &s) == 0);
    drop(&s);
    service_relexec_keep = keep_t;
    assert(load("exec=relative/path\n", NULL, &s) == 0 && !strcmp(s.exec, "relative/path"));
    drop(&s);
    put(path, "exec=rel\nname=other\n");
    assert(load_file(path, &s) != 0);
    put(path, "exec=rel\nname=t\n");
    assert(load_file(path, &s) == 0);
    drop(&s);
    put(path, "exec=rel\n");                   /* no name=: "t" from t.svc */
    assert(load_file(path, &s) == 0 && !strcmp(s.name, "t"));
    drop(&s);
    {
        char ip[320];
        snprintf(ip, sizeof ip, "%s/m@1.svc", dir);
        put(ip, "name=m\nexec=rel\n");
        assert(load_file(ip, &s) != 0);          /* keep_t knows "t", not "m@1" */
        service_relexec_keep = keep_m1;
        assert(load_file(ip, &s) == 0 && !strcmp(s.name, "m@1"));
        drop(&s);
        unlink(ip);
    }
    service_relexec_keep = NULL;
    put(path, "exec=rel\nname=t\n");
    assert(load_file(path, &s) != 0);

    unlink(path);
    rmdir(dir);
    printf("test_service_order: ok\n");
    return 0;
}
