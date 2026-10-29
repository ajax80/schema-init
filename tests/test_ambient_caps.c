#include "../caps.h"
#include <assert.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static unsigned long long status_hex(const char *key) {
    char line[256];
    unsigned long long v = 0;
    size_t kl = strlen(key);
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':')
            v = strtoull(line + kl + 1, NULL, 16);
    fclose(f);
    return v;
}

int main(void) {
    if (geteuid() != 0) {
        printf("skip: not root\n");
        return 0;
    }
    struct passwd *pw = getpwnam("nobody");
    assert(pw && pw->pw_uid != 0);
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;

    int pfd[2];
    assert(pipe(pfd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        const char *step = "";
        close(pfd[0]);
        if (apply_ambient_caps(1ULL << 10, uid, gid, "nobody", &step) != 0) {
            fprintf(stderr, "apply_ambient_caps: %s\n", step);
            _exit(2);
        }
        if (getuid() != uid || geteuid() != uid || getgid() != gid) _exit(3);
        if (status_hex("CapAmb") != (1ULL << 10)) _exit(4);
        if (!(status_hex("CapEff") & (1ULL << 10))) _exit(5);
        dup2(pfd[1], STDOUT_FILENO);
        /* surviving exec as non-root is the whole point of ambient caps */
        execl("/bin/sh", "sh", "-c", "grep -E '^Cap(Amb|Eff):' /proc/self/status", (char *)NULL);
        _exit(6);
    }
    close(pfd[1]);
    char out[512];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof out - 1 && (r = read(pfd[0], out + n, sizeof out - 1 - n)) > 0) n += r;
    out[n] = '\0';
    close(pfd[0]);
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "ambient-caps child failed: status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    char *amb = strstr(out, "CapAmb:"), *eff = strstr(out, "CapEff:");
    assert(amb && eff);
    assert(strtoull(amb + 7, NULL, 16) & (1ULL << 10));
    assert(strtoull(eff + 7, NULL, 16) & (1ULL << 10));
    printf("all ambient-caps tests passed\n");
    return 0;
}
