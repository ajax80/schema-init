#include "../service.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[] = "/tmp/schema-svcwatch-XXXXXX";
static char path[300], errf[300];
static char err[2048];

/* load keys; stderr of the load lands in err */
static int load(const char *keys, service_t *s) {
    char body[4096];
    FILE *f = fopen(path, "w");
    assert(f);
    snprintf(body, sizeof body, "name=t\nexec=/bin/true\n%s", keys);
    fputs(body, f);
    fclose(f);
    memset(s, 0, sizeof *s);
    fflush(stderr);
    int saved = dup(2), e = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(e, 2);
    close(e);
    int r = service_load_one(path, s);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    f = fopen(errf, "r");
    size_t n = fread(err, 1, sizeof err - 1, f);
    err[n] = 0;
    fclose(f);
    return r;
}

static void refuse(const char *keys, const char *needle) {
    static service_t s;
    if (load(keys, &s) != -1) { fprintf(stderr, "accepted: %s", keys); exit(1); }
    if (!strstr(err, needle)) { fprintf(stderr, "refusal of %s does not name %s: %s", keys, needle, err); exit(1); }
}

int main(void) {
    static service_t s;
    char keys[2048];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/t.svc", dir);
    snprintf(errf, sizeof errf, "%s/err", dir);

    assert(load("", &s) == 0 && !s.watch_count && !(s.flags & SVC_PATH));
    assert(load("watch_path=exists:/run/x\n", &s) == 0);
    assert(s.watch_count == 1 && (s.flags & SVC_PATH) && s.watch[0].kind == WATCH_EXISTS &&
           !strcmp(s.watch[0].path, "/run/x"));
    assert(s.directory_mode == 0755 && s.trigger_burst == 200 && s.trigger_interval_sec == 2 &&
           !s.make_directory);

    keys[0] = 0;
    for (int i = 0; i < MAX_WATCH; i++)
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "watch_path=changed:/run/w%d\n", i);
    assert(load(keys, &s) == 0 && s.watch_count == MAX_WATCH && s.watch[7].kind == WATCH_CHANGED);
    snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "watch_path=changed:/run/w9\n");
    refuse(keys, "watch_path");

    /* a drop-in's empty value resets the list */
    assert(load("watch_path=exists:/a\nwatch_path=\nwatch_path=modified:/b\n", &s) == 0 &&
           s.watch_count == 1 && s.watch[0].kind == WATCH_MODIFIED);
    assert(load("watch_path=exists:/a\nwatch_path=\n", &s) == 0 && !s.watch_count && !(s.flags & SVC_PATH));

    assert(load("watch_path=dir_not_empty:/run/q\nmake_directory=1\ndirectory_mode=0700\n"
                "trigger_limit_burst=4\ntrigger_limit_interval_sec=1800\n", &s) == 0);
    assert(s.make_directory && s.directory_mode == 0700 && s.trigger_burst == 4 &&
           s.trigger_interval_sec == 1800);

    char big[400];
    memset(big, 'x', sizeof big);
    snprintf(big, sizeof big, "watch_path=exists:/%0300d\n", 0);
    refuse("watch_path=exists:rel\n", "watch_path");
    refuse(big, "watch_path");
    refuse("watch_path=exists:/a\ndirectory_mode=0999\n", "directory_mode");
    refuse("watch_path=exists:/a\ntrigger_limit_burst=0\n", "trigger_limit_burst");
    refuse("watch_path=exists:/a\ntrigger_limit_interval_sec=0\n", "trigger_limit_interval_sec");
    refuse("watch_path=exists:/a\nlisten=stream:/run/s\n", "watch_path");
    refuse("watch_path=exists:/a\non_boot_sec=5\n", "watch_path");
    refuse("watch_path=exists:/a\non_active_sec=5\n", "watch_path");
    refuse("watch_path=exists:/a\non_calendar=03:00\n", "watch_path");

    unlink(path);
    unlink(errf);
    rmdir(dir);
    printf("all svc watch tests passed\n");
    return 0;
}
