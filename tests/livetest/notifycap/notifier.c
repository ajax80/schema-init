#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int say(const char *m) {
    const char *p = getenv("NOTIFY_SOCKET");
    if (!p) return 2;
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, p, sizeof a.sun_path - 1);
    int r = sendto(s, m, strlen(m), 0, (struct sockaddr *)&a, sizeof a) < 0;
    close(s);
    return r;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (argc > 2 && !strcmp(argv[2], "pets")) {
        say(argv[1]);
        for (;;) { say("WATCHDOG=1"); sleep(1); }
    }
    if (argc > 2 && !strcmp(argv[2], "hold")) {
        say(argv[1]);
        for (;;) pause();
    }
    return say(argv[1]);
}
