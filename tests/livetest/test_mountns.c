/* test_mountns — livetest helper for mount-namespace hardening. Probes, from
 * inside the service, every isolation the .svc claims and writes one report
 * line to /run/mountns-<tag>. Run once hardened and once plain: the plain run
 * must report every probe OPEN, or the probes are testing nothing. Then waits
 * for the host to mount over /home and re-checks the hidden view. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

static const char *probe_write(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { close(fd); unlink(path); return "OPEN"; }
    return errno == EROFS ? "RO" : "ERR";
}

static const char *probe_empty(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return "ERR";
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n ? "OPEN" : "EMPTY";
}

int main(int argc, char **argv) {
    const char *tag = argc > 1 ? argv[1] : "x";
    char path[128], sentinel[128];
    FILE *f;
    int fd;

    snprintf(sentinel, sizeof(sentinel), "/tmp/mountns-sentinel-%s", tag);
    fd = open(sentinel, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);

    snprintf(path, sizeof(path), "/run/mountns-%s", tag);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "usr=%s etc=%s efi=%s home=%s root=%s tmpwrite=%s\n",
            probe_write("/usr/.probe"), probe_write("/etc/.probe"),
            probe_write("/boot/efi/.probe"), probe_empty("/home"),
            probe_empty("/root"), fd >= 0 ? "OK" : "ERR");
    fclose(f);

    while (access("/run/mountns-recheck", F_OK) != 0) sleep(1);
    snprintf(path, sizeof(path), "/run/mountns-late-%s", tag);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "home=%s\n", probe_empty("/home"));
    fclose(f);

    for (;;) pause();
}
