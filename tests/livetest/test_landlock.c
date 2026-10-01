/* test_landlock — livetest helper for landlock_ro= / landlock_rw=. Probes
 * from inside the service and prints one LL line to its log. Run once
 * confined and once plain: the plain run must report every probe OK, or the
 * probes are testing nothing. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>

static const char *res(int ok) {
    if (ok) return "OK";
    return errno == EACCES ? "DENIED" : strerror(errno);
}

static const char *probe_read(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd >= 0) close(fd);
    return res(fd >= 0);
}

static const char *probe_write(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { close(fd); unlink(p); }
    return res(fd >= 0);
}

static const char *probe_dir(const char *p) {
    DIR *d = opendir(p);
    if (d) closedir(d);
    return res(d != NULL);
}

static const char *probe_exec(const char *p) {
    static char buf[32];
    int st;
    pid_t pid = fork();
    if (pid == 0) {
        execl(p, "true", (char *)NULL);
        _exit(errno == EACCES ? 126 : 127);
    }
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return "OK";
    if (WIFEXITED(st) && WEXITSTATUS(st) == 126) return "DENIED";
    snprintf(buf, sizeof buf, "st%d", st);
    return buf;
}

int main(int argc, char **argv) {
    const char *tag = argc > 1 ? argv[1] : "x";
    printf("LL %s: uid=%d rd_in=%s rd_out=%s wr_rw=%s wr_ro=%s dir_out=%s exec_in=%s exec_out=%s\n",
           tag, (int)getuid(), probe_read("/etc/ll/conf"), probe_read("/etc/ll-other"),
           probe_write("/srv/ll/probe"), probe_write("/etc/ll/probe"),
           probe_dir("/root"), probe_exec("/bin/true"), probe_exec("/usr/libexec/true"));
    fflush(stdout);
    for (;;) pause();
}
