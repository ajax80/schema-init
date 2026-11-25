#include "../service.h"
#include "../schema.h"
#include <assert.h>
#include <stdio.h>
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
        for (int d = 0; d < MAX_DEPS; d++) t[i].dep_idx[d] = -1;
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
    /* an empty after= in a drop-in clears the base file's list */
    assert(load("after=a\nbefore=b\n", "after=\nafter=z\nbefore=\n", &s) == 0);
    assert(!strcmp(s.after_name[0], "z") && !s.after_name[1][0] && !s.before_name[0][0]);

    /* resolve: own after= plus another's before=, unique; unknown names ignored */
    reset(3); after(0, 1); after(0, 1); before(1, 0); before(2, 0);
    snprintf(t[0].after_name[2], 64, "missing");
    services_resolve_order(t, 3);
    assert(t[0].order_count == 2 && has_edge(0, 1) && has_edge(0, 2));
    assert(!t[1].order_count && !t[2].order_count);

    /* a cycle of order edges keeps the first and drops the closing one */
    reset(2); after(0, 1); after(1, 0);
    services_resolve_order(t, 2);
    assert(has_edge(0, 1) && !t[1].order_count);
    /* a cycle through a dep= drops the order edge, never the dep */
    reset(3); t[0].dep_idx[0] = 1; after(1, 2); after(2, 0);
    services_resolve_order(t, 3);
    assert(has_edge(1, 2) && !t[2].order_count);
    reset(2); after(0, 0); before(1, 1);
    services_resolve_order(t, 2);
    assert(!t[0].order_count && !t[1].order_count);
    /* re-resolving (a reload) starts over rather than appending */
    reset(2); after(0, 1);
    services_resolve_order(t, 2);
    services_resolve_order(t, 2);
    assert(t[0].order_count == 1);

    /* ready: queued or spawned-not-ready holds; anything settled either way lets go */
    reset(2); after(0, 1);
    services_resolve_order(t, 2);
    static const struct { uint8_t st; int restarts, ready; } c[] = {
        { STATE_NEW_PROCESS, 0, 0 }, { STATE_FULL_TRUST, 0, 0 }, { STATE_FULL_TRUST, 2, 0 },
        { STATE_NEW_PROCESS, 1, 1 }, { STATE_FUNDAMENTAL, 0, 1 }, { STATE_SETTLED, 0, 1 },
        { STATE_PERFECT, 0, 1 }, { STATE_RECOVERY, 1, 1 }, { STATE_FRICTION, 3, 1 },
        { STATE_DORMANT, 3, 1 }, { STATE_EXCISED, 0, 1 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        t[1].inst.state = c[i].st;
        t[1].restart_count = c[i].restarts;
        assert(service_order_ready(&t[0], t, 2) == c[i].ready);
    }
    reset(1);
    assert(service_order_ready(&t[0], t, 1));

    rmdir(dir);
    printf("test_service_order: ok\n");
    return 0;
}
