/* Live interop client for schema-dbus: two real libdbus connections exercise
   Hello/RequestName (driver) and a client->client method call with a reply that
   carries a unix fd (routing + reply-tracking + SCM_RIGHTS). Connects to
   $SCHEMA_DBUS_SOCKET. Exits 0 on success, non-zero with a message on failure. */

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#define FAIL(...) do { fprintf(stderr, "interop FAIL: " __VA_ARGS__); return 1; } while (0)

int main(void) {
    const char *sock = getenv("SCHEMA_DBUS_SOCKET");
    if (!sock) { fprintf(stderr, "SCHEMA_DBUS_SOCKET unset\n"); return 2; }
    char addr[512];
    snprintf(addr, sizeof addr, "unix:path=%s", sock);

    DBusError e; dbus_error_init(&e);
    DBusConnection *A = dbus_connection_open_private(addr, &e);
    if (!A) FAIL("open A: %s\n", e.message);
    if (!dbus_bus_register(A, &e)) FAIL("register A: %s\n", e.message);
    DBusConnection *B = dbus_connection_open_private(addr, &e);
    if (!B) FAIL("open B: %s\n", e.message);
    if (!dbus_bus_register(B, &e)) FAIL("register B: %s\n", e.message);

    const char *aname = dbus_bus_get_unique_name(A);
    printf("  A unique = %s\n", aname);
    if (!aname || aname[0] != ':') FAIL("A has no unique name\n");

    /* A requests a well-known name */
    int rn = dbus_bus_request_name(A, "org.test.Server",
                                   DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (rn != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        FAIL("RequestName rc=%d %s\n", rn, dbus_error_is_set(&e) ? e.message : "");
    printf("  A owns org.test.Server\n");

    /* B resolves the owner -> must be A's unique */
    const char *owner = NULL;
    DBusMessage *gno = dbus_message_new_method_call("org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
    const char *wk = "org.test.Server";
    dbus_message_append_args(gno, DBUS_TYPE_STRING, &wk, DBUS_TYPE_INVALID);
    DBusMessage *gnor = dbus_connection_send_with_reply_and_block(B, gno, 2000, &e);
    if (!gnor) FAIL("GetNameOwner: %s\n", e.message);
    if (!dbus_message_get_args(gnor, &e, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID))
        FAIL("GetNameOwner args: %s\n", e.message);
    if (strcmp(owner, aname) != 0) FAIL("owner %s != A %s\n", owner, aname);
    printf("  B sees org.test.Server owned by %s\n", owner);
    dbus_message_unref(gno); dbus_message_unref(gnor);

    /* B calls A; A replies with a string AND a unix fd. Manual pump so one
       process can drive both connections. */
    DBusMessage *call = dbus_message_new_method_call("org.test.Server", "/",
                                                     "org.test.If", "Echo");
    const char *payload = "hello-through-the-bus";
    dbus_message_append_args(call, DBUS_TYPE_STRING, &payload, DBUS_TYPE_INVALID);
    dbus_uint32_t serial = 0;
    if (!dbus_connection_send(B, call, &serial)) FAIL("send call\n");
    dbus_connection_flush(B);
    dbus_message_unref(call);

    int got_reply = 0, fd_ok = 0;
    for (int i = 0; i < 200 && !got_reply; i++) {
        /* A: service the call */
        dbus_connection_read_write_dispatch(A, 50);
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(A))) {
            if (dbus_message_is_method_call(m, "org.test.If", "Echo")) {
                const char *in = NULL;
                dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &in, DBUS_TYPE_INVALID);
                DBusMessage *rep = dbus_message_new_method_return(m);
                int fd = open("/dev/null", O_RDONLY);
                dbus_message_append_args(rep, DBUS_TYPE_STRING, &in,
                                         DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
                dbus_connection_send(A, rep, NULL);
                dbus_connection_flush(A);
                dbus_message_unref(rep);
                close(fd);
            }
            dbus_message_unref(m);
        }
        /* B: collect the reply */
        dbus_connection_read_write_dispatch(B, 50);
        while ((m = dbus_connection_pop_message(B))) {
            if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
                dbus_message_get_reply_serial(m) == serial) {
                const char *out = NULL; int rfd = -1;
                if (dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &out,
                                          DBUS_TYPE_UNIX_FD, &rfd, DBUS_TYPE_INVALID)) {
                    if (out && !strcmp(out, payload)) got_reply = 1;
                    struct stat st;
                    if (rfd >= 0 && fstat(rfd, &st) == 0) { fd_ok = 1; close(rfd); }
                }
            }
            dbus_message_unref(m);
        }
    }
    if (!got_reply) FAIL("no reply routed back to B\n");
    printf("  B got reply payload echoed\n");
    if (!fd_ok) FAIL("unix fd not received across the bus\n");
    printf("  B received a live unix fd (SCM_RIGHTS)\n");

    dbus_connection_close(A); dbus_connection_unref(A);
    dbus_connection_close(B); dbus_connection_unref(B);
    printf("interop OK\n");
    return 0;
}
