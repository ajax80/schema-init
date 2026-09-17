#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 500;
    int warm = argc > 2 ? atoi(argv[2]) : 50;
    DBusError err;
    dbus_error_init(&err);
    const char *b = getenv("PROBE_BUS");
    DBusBusType bt = (b && b[0] == 's' && b[1] == 'e') ? DBUS_BUS_SESSION : DBUS_BUS_SYSTEM;
    DBusConnection *c = dbus_bus_get(bt, &err);
    if (!c) {
        fprintf(stderr, "connect: %s\n", err.message);
        return 1;
    }
    double *us = malloc(sizeof(double) * n);
    struct timespec t0, t1;
    for (int i = 0; i < n + warm; i++) {
        DBusMessage *m = dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus.Peer", "Ping");
        clock_gettime(CLOCK_MONOTONIC, &t0);
        DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        dbus_message_unref(m);
        if (!r) {
            fprintf(stderr, "ping %d: %s\n", i, err.message);
            return 2;
        }
        dbus_message_unref(r);
        if (i >= warm)
            us[i - warm] = (t1.tv_sec - t0.tv_sec) * 1e6 +
                           (t1.tv_nsec - t0.tv_nsec) / 1e3;
    }
    qsort(us, n, sizeof(double), cmp_d);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += us[i];
    printf("n=%d min=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f mean=%.1f\n",
           n, us[0], us[(int)(n * 0.50)], us[(int)(n * 0.90)],
           us[(int)(n * 0.99)], us[n - 1], sum / n);
    return 0;
}
