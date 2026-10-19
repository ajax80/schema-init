#include "../landlock.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/landlock.h>

static int sock_at(int type, int port, int do_listen) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, type, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { int e = errno; close(s); errno = e; return -1; }
    if (do_listen) listen(s, 4);
    return s;
}

static int port_of(int s) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &l);
    return ntohs(a.sin_port);
}

static int try_bind(int type, int port) {
    int s = sock_at(type, port, 0);
    if (s < 0) return errno;
    close(s);
    return 0;
}

static int try_connect(int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0), r = connect(s, (struct sockaddr *)&a, sizeof a), e = errno;
    close(s);
    return r == 0 ? 0 : e;
}

static int free_port(void) {
    int s = sock_at(SOCK_STREAM, 0, 0), p = port_of(s);
    close(s);
    return p;
}

static int confined(const int n[2], const uint16_t ports[2][MAX_LANDLOCK]) {
    const char *step;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    return apply_landlock(NULL, 0, 0, n, ports, &step);
}

int main(void) {
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 4) {
        printf("landlock-tcp: skipped (Landlock ABI %ld, TCP rules need 4)\n", abi);
        return 0;
    }
    int la = sock_at(SOCK_STREAM, 0, 1), lb = sock_at(SOCK_STREAM, 0, 1);
    int pa = port_of(la), pb = port_of(lb), b1 = free_port(), b2 = free_port();
    assert(la >= 0 && lb >= 0 && pa != pb && b1 != b2);

    pid_t pid = fork();
    if (pid == 0) {
        int n[2] = { 1, 1 };
        uint16_t ports[2][MAX_LANDLOCK] = { { b1 }, { pa } };
        if (confined(n, ports) != 0) _exit(10);
        if (try_bind(SOCK_STREAM, b1) != 0) _exit(11);
        if (try_bind(SOCK_STREAM, b2) != EACCES) _exit(12);
        if (try_connect(pa) != 0) _exit(13);
        if (try_connect(pb) != EACCES) _exit(14);
        if (try_bind(SOCK_DGRAM, b2) != 0) _exit(15);
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fprintf(stderr, "bind+connect child: status %d\n", WEXITSTATUS(st));
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pid = fork();
    if (pid == 0) {
        int n[2] = { 0, -1 };
        uint16_t ports[2][MAX_LANDLOCK] = { { 0 }, { 0 } };
        if (confined(n, ports) != 0) _exit(10);
        if (try_bind(SOCK_STREAM, b1) != EACCES) _exit(11);
        if (try_connect(pb) != 0) _exit(12);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fprintf(stderr, "none+unrestricted child: status %d\n", WEXITSTATUS(st));
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    close(la);
    close(lb);
    printf("landlock-tcp: all passed (ABI %ld)\n", abi);
    return 0;
}
