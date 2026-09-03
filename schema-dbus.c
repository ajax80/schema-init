#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int system_bus = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--system")) system_bus = 1;
    int maj = 0, min = 0, mic = 0;
    dbus_get_version(&maj, &min, &mic);
    fprintf(stderr, "schema-dbus: starting (system=%d, libdbus %d.%d.%d)\n",
            system_bus, maj, min, mic);
    return 0;
}
