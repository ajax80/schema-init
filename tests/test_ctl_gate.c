#include "../ctl_gate.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    static const char *ro[] = { "status", "status --json", "status ci-daemon", "list",
                                "timing", "analyze", "analyze\tcritical-chain" };
    static const char *rw[] = { "start x", "stop x", "restart x", "reexec", "reexec --force",
                                "poweroff", "reboot", "reload", "enable x", "kill x",
                                "statusfoo", "status;stop x", "liststop", "analyzer", "",
                                " status", "STATUS", "stat" };
    for (size_t i = 0; i < sizeof ro / sizeof *ro; i++) {
        assert(ctl_is_readonly(ro[i]));
        assert(ctl_peer_may(1000, 0, ro[i]));
    }
    for (size_t i = 0; i < sizeof rw / sizeof *rw; i++) {
        assert(!ctl_is_readonly(rw[i]));
        /* group schema member: refused */
        assert(!ctl_peer_may(1000, 0, rw[i]));
        /* root, and PID 1's own euid (schema-user) run everything */
        assert(ctl_peer_may(0, 0, rw[i]));
        assert(ctl_peer_may(1000, 1000, rw[i]));
        assert(ctl_peer_may(0, 1000, rw[i]));
        assert(!ctl_peer_may(1001, 1000, rw[i]));
    }
    printf("test_ctl_gate: ok\n");
    return 0;
}
