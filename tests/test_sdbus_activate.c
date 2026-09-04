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

int main(void) {
    test_parse();
    printf("all sdbus_activate tests passed\n");
    return 0;
}
