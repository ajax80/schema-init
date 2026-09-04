#include "../sdbus_activate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void) {
    test_parse();
    test_pending();
    printf("all sdbus_activate tests passed\n");
    return 0;
}
