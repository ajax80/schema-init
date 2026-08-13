#ifndef SCHEMA_UDEV_EXEC_H
#define SCHEMA_UDEV_EXEC_H
#include "schema-udev.h"
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

static inline int udev_argv_split(const char *cmd, char store[][UE_VAL_MAX],
                                  char *argv[], int max) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max) return -1;
        char *o = store[argc]; size_t n = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') { p++; while (*p && *p != '\'') { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
                              if (*p == '\'') p++; }
            else { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
        }
        o[n] = '\0'; argv[argc] = store[argc]; argc++;
    }
    argv[argc] = NULL;
    return argc;
}

#define UDEV_EXEC_TIMEOUT 180
#define UDEV_EXEC_LIBDIR  "/usr/lib/udev"

static inline int udev_run_capture(const char *cmd, char *out, size_t outlen) {
    if (outlen) out[0] = '\0';
    char store[32][UE_VAL_MAX]; char *argv[33];
    int argc = udev_argv_split(cmd, store, argv, 32);
    if (argc <= 0) return -1;

    char libpath[PATH_MAX];
    if (!strchr(argv[0], '/')) {
        snprintf(libpath, sizeof libpath, "%s/%s", UDEV_EXEC_LIBDIR, argv[0]);
        if (access(libpath, X_OK) == 0) argv[0] = libpath;
    }

    int p[2], errp[2];
    if (pipe(p) != 0) return -1;
    if (pipe(errp) != 0) { close(p[0]); close(p[1]); return -1; }
    fcntl(errp[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); close(errp[0]); close(errp[1]); return -1; }
    if (pid == 0) {
        close(errp[0]);
        dup2(p[1], STDOUT_FILENO);
        int nf = open("/dev/null", O_WRONLY);
        if (nf >= 0) { dup2(nf, STDERR_FILENO); close(nf); }
        close(p[0]); close(p[1]);
        execvp(argv[0], argv);
        int e = errno;
        ssize_t wr = write(errp[1], &e, sizeof e); (void)wr;
        _exit(127);
    }
    close(p[1]); close(errp[1]);

    int child_errno = 0;
    ssize_t er = read(errp[0], &child_errno, sizeof child_errno);
    close(errp[0]);
    int status;
    if (er > 0) {
        close(p[0]);
        waitpid(pid, &status, 0);
        return -1;
    }

    fcntl(p[0], F_SETFL, O_NONBLOCK);
    size_t o = 0;
    int timed_out = 0;
    time_t start = time(NULL);
    for (;;) {
        char buf[4096]; ssize_t r = read(p[0], buf, sizeof buf);
        if (r > 0) {
            for (ssize_t i = 0; i < r && o + 1 < outlen; i++) out[o++] = buf[i];
            continue;
        }
        if (r == 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (time(NULL) - start >= UDEV_EXEC_TIMEOUT) { timed_out = 1; break; }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    close(p[0]);
    if (o < outlen) out[o] = '\0';
    if (timed_out) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return -1; }
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif
