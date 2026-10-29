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

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (apply_ambient_caps(1ULL << 10, uid, gid, "nobody") != 0) _exit(2);
        if (getuid() != uid || geteuid() != uid || getgid() != gid) _exit(3);
        if (!(status_hex("CapAmb") & (1ULL << 10))) _exit(4);
        if (!(status_hex("CapEff") & (1ULL << 10))) _exit(5);
        if (status_hex("CapAmb") != (1ULL << 10)) _exit(6);
        _exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "ambient-caps child failed: status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    printf("all ambient-caps tests passed\n");
    return 0;
}
