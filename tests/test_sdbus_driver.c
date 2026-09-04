#include "../sdbus_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_ntrans;
static sdbus_transition g_trans[8];
static void record_broadcast(void *ctx, sdbus_transition *t, int n) {
    (void)ctx;
    for (int i = 0; i < n && g_ntrans < 8; i++) g_trans[g_ntrans++] = t[i];
}

/* dispatch one call against conn c (in the singleton `all`); returns the reply
   demarshalled from c->out (which is reset first). */
static sdbus_msg do_call(sdbus_conn *c, sdbus_names *names, DBusMessage *call) {
    c->out_len = 0;
    g_ntrans = 0;
    if (c->unique) dbus_message_set_sender(call, c->unique);
    dbus_message_set_serial(call, 100);
    sdbus_msg cm; memset(&cm, 0, sizeof cm); cm.msg = call; cm.member = dbus_message_get_member(call);
    sdbus_conn *all[] = { c };
    int rc = sdbus_driver_dispatch(&cm, c, names, all, 1, record_broadcast, NULL);
    assert(rc == 0);
    sdbus_msg reply;
    int taken = sdbus_codec_take(c->out, c->out_len, &reply);
    assert(taken == c->out_len && taken > 0);
    return reply;
}

static DBusMessage *mkcall(const char *member) {
    return dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                        SDBUS_DRIVER_NAME, member);
}

static int strv_has(sdbus_msg *m, const char *want) {
    DBusMessageIter it, arr;
    if (!dbus_message_iter_init(m->msg, &it)) return 0;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return 0;
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *s; dbus_message_iter_get_basic(&arr, &s);
        if (!strcmp(s, want)) return 1;
        dbus_message_iter_next(&arr);
    }
    return 0;
}

int main(void) {
    sdbus_names *names = sdbus_names_new();
    sdbus_conn c; memset(&c, 0, sizeof c); c.id = 1;

    /* Hello -> unique name, said_hello set */
    DBusMessage *hello = mkcall("Hello");
    sdbus_msg r = do_call(&c, names, hello);
    assert(c.said_hello == 1);
    assert(c.unique && !strcmp(c.unique, ":1.1"));
    const char *u = NULL; DBusError e; dbus_error_init(&e);
    assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_STRING, &u, DBUS_TYPE_INVALID));
    assert(!strcmp(u, ":1.1"));
    sdbus_msg_free(&r); dbus_message_unref(hello);

    /* RequestName(org.x, DO_NOT_QUEUE) on a free name -> PRIMARY_OWNER + transition */
    DBusMessage *rn = mkcall("RequestName");
    const char *nm = "org.x"; dbus_uint32_t flags = SDBUS_REQ_DO_NOT_QUEUE;
    dbus_message_append_args(rn, DBUS_TYPE_STRING, &nm, DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID);
    r = do_call(&c, names, rn);
    dbus_uint32_t code = 0;
    assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID));
    assert(code == SDBUS_REQ_PRIMARY_OWNER);
    assert(g_ntrans == 1 && g_trans[0].old_owner == -1 && g_trans[0].new_owner == 1);
    sdbus_msg_free(&r); dbus_message_unref(rn);

    /* GetNameOwner(org.x) -> our unique */
    DBusMessage *gno = mkcall("GetNameOwner");
    dbus_message_append_args(gno, DBUS_TYPE_STRING, &nm, DBUS_TYPE_INVALID);
    r = do_call(&c, names, gno);
    const char *owner = NULL;
    assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID));
    assert(!strcmp(owner, ":1.1"));
    sdbus_msg_free(&r); dbus_message_unref(gno);

    /* NameHasOwner(org.missing) -> false */
    DBusMessage *nho = mkcall("NameHasOwner");
    const char *miss = "org.missing";
    dbus_message_append_args(nho, DBUS_TYPE_STRING, &miss, DBUS_TYPE_INVALID);
    r = do_call(&c, names, nho);
    dbus_bool_t has = TRUE;
    assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_BOOLEAN, &has, DBUS_TYPE_INVALID));
    assert(has == FALSE);
    sdbus_msg_free(&r); dbus_message_unref(nho);

    /* ListNames includes the driver and org.x */
    DBusMessage *ln = mkcall("ListNames");
    r = do_call(&c, names, ln);
    assert(strv_has(&r, SDBUS_DRIVER_NAME));
    assert(strv_has(&r, "org.x"));
    assert(strv_has(&r, ":1.1"));
    sdbus_msg_free(&r); dbus_message_unref(ln);

    /* AddMatch installs a rule and returns empty */
    DBusMessage *am = mkcall("AddMatch");
    const char *rule = "type='signal',interface='org.freedesktop.DBus'";
    dbus_message_append_args(am, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID);
    r = do_call(&c, names, am);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    assert(c.matches && sdbus_match_signal(c.matches, "org.freedesktop.DBus", "X", "/p", "s", NULL, 0, NULL) == 1);
    sdbus_msg_free(&r); dbus_message_unref(am);

    /* GetConnectionCredentials(:1.1) -> a{sv} carrying UnixUserID, ProcessID,
       UnixGroupIDs (the 60x-dominant credentials method on the live bus) */
    c.uid = 1000; c.pid = 4242;
    c.gids[0] = 1000; c.gids[1] = 10; c.n_gids = 2;
    DBusMessage *gcc = mkcall("GetConnectionCredentials");
    const char *self = ":1.1";
    dbus_message_append_args(gcc, DBUS_TYPE_STRING, &self, DBUS_TYPE_INVALID);
    r = do_call(&c, names, gcc);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    {
        DBusMessageIter it, arr;
        assert(dbus_message_iter_init(r.msg, &it));
        assert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY);
        int saw_uid = 0, saw_pid = 0, saw_gids = 0;
        dbus_uint32_t uid = 0, pid = 0; int ngids = 0;
        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent, var;
            dbus_message_iter_recurse(&arr, &ent);
            const char *key = NULL;
            dbus_message_iter_get_basic(&ent, &key);
            dbus_message_iter_next(&ent);
            dbus_message_iter_recurse(&ent, &var);
            if (!strcmp(key, "UnixUserID")) { dbus_message_iter_get_basic(&var, &uid); saw_uid = 1; }
            else if (!strcmp(key, "ProcessID")) { dbus_message_iter_get_basic(&var, &pid); saw_pid = 1; }
            else if (!strcmp(key, "UnixGroupIDs")) {
                DBusMessageIter au; dbus_message_iter_recurse(&var, &au);
                while (dbus_message_iter_get_arg_type(&au) == DBUS_TYPE_UINT32) {
                    ngids++; dbus_message_iter_next(&au);
                }
                saw_gids = 1;
            }
            dbus_message_iter_next(&arr);
        }
        assert(saw_uid && uid == 1000);
        assert(saw_pid && pid == 4242);
        assert(saw_gids && ngids == 2);
    }
    sdbus_msg_free(&r); dbus_message_unref(gcc);

    /* GetConnectionCredentials for an unowned name -> NameHasNoOwner */
    DBusMessage *gccn = mkcall("GetConnectionCredentials");
    dbus_message_append_args(gccn, DBUS_TYPE_STRING, &miss, DBUS_TYPE_INVALID);
    r = do_call(&c, names, gccn);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_ERROR);
    assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_NAME_HAS_NO_OWNER));
    sdbus_msg_free(&r); dbus_message_unref(gccn);

    /* StartServiceByName -> ServiceUnknown error (v1.1 deferral) */
    DBusMessage *ssn = mkcall("StartServiceByName");
    const char *svc = "org.example.svc"; dbus_uint32_t z = 0;
    dbus_message_append_args(ssn, DBUS_TYPE_STRING, &svc, DBUS_TYPE_UINT32, &z, DBUS_TYPE_INVALID);
    r = do_call(&c, names, ssn);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_ERROR);
    assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_SERVICE_UNKNOWN));
    sdbus_msg_free(&r); dbus_message_unref(ssn);

    /* Ping -> empty return */
    DBusMessage *ping = mkcall("Ping");
    r = do_call(&c, names, ping);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    sdbus_msg_free(&r); dbus_message_unref(ping);

    /* unknown member -> dispatch returns -1 */
    DBusMessage *bogus = mkcall("NoSuchMethod");
    dbus_message_set_sender(bogus, c.unique);
    dbus_message_set_serial(bogus, 200);
    sdbus_msg bm; memset(&bm, 0, sizeof bm); bm.msg = bogus; bm.member = "NoSuchMethod";
    sdbus_conn *all[] = { &c };
    assert(sdbus_driver_dispatch(&bm, &c, names, all, 1, record_broadcast, NULL) == -1);
    dbus_message_unref(bogus);

    sdbus_conn_free_fields(&c);
    sdbus_names_free(names);
    printf("all sdbus_driver tests passed\n");
    return 0;
}
