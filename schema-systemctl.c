#include "systemctl_shim.h"

int main(int argc, char **argv) {
    return shim_dispatch(argc, argv);
}
