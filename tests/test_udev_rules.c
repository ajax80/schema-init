#include "udev_rules.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- synthetic sysfs builder (same pattern as test_udev_builtins.c) --- */
static char ROOT[64];
static void root_make(void) { strcpy(ROOT, "/tmp/urtestXXXXXX"); assert(mkdtemp(ROOT)); }
static void mkdirs(const char *rel) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void writef(const char *rel, const char *body) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}
static void set_subsystem(const char *devrel, const char *name) {
    char busrel[PATH_MAX]; snprintf(busrel, sizeof busrel, "/bus/%s", name); mkdirs(busrel);
    char linkp[PATH_MAX], target[PATH_MAX];
    snprintf(linkp, sizeof linkp, "%s%s/subsystem", ROOT, devrel);
    snprintf(target, sizeof target, "%s/bus/%s", ROOT, name);
    unlink(linkp); assert(symlink(target, linkp) == 0);
}
static const char *getval(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

static void test_inert_on_childless(void) {
    root_make();
    /* a lone device with a uevent file and no interesting ancestors/modalias */
    mkdirs("/devices/virtual/mem/null");
    writef("/devices/virtual/mem/null/uevent", "DEVTYPE=\nMAJOR=1\nMINOR=3\n");
    set_subsystem("/devices/virtual/mem/null", "mem");
    struct uevent ev; ev.n = 0;
    strcpy(ev.key[ev.n], "SUBSYSTEM"); strcpy(ev.val[ev.n], "mem"); ev.n++;
    strcpy(ev.key[ev.n], "DEVPATH");   strcpy(ev.val[ev.n], "/devices/virtual/mem/null"); ev.n++;
    int before = ev.n;
    int added = run_rules(ROOT, "/devices/virtual/mem/null", "/dev/null", &ev);
    assert(added == 0);
    assert(ev.n == before);
    (void)getval;
    printf("test_udev_rules inert: OK\n");
}

int main(void) {
    test_inert_on_childless();
    return 0;
}
