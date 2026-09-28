#include "systemctl_shim.h"

int main(int argc, char **argv) {
    const char *real = shim_passthrough();
    if (real) {
        execv(real, argv);
        perror("schema-systemctl: exec real systemctl");
    }
    return shim_dispatch(argc, argv);
}
