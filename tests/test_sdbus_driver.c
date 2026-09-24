#include "../sdbus_driver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_ntrans;
static const sdbus_svctab *g_tab;
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
    int rc = sdbus_driver_dispatch(&cm, c, names, all, 1, g_tab, record_broadcast, NULL);
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

    /* Introspect -> XML naming the driver interface */
    DBusMessage *intro = dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                                      "org.freedesktop.DBus.Introspectable", "Introspect");
    r = do_call(&c, names, intro);
    const char *xml = NULL;
    assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID));
    assert(strstr(xml, "<interface name=\"org.freedesktop.DBus\">"));
    assert(strstr(xml, "ListQueuedOwners"));
    sdbus_msg_free(&r); dbus_message_unref(intro);

    /* Properties: Get Features -> variant(as), GetAll -> 2 entries,
       Set -> PropertyReadOnly, unknown -> UnknownProperty */
    const char *dif = SDBUS_DRIVER_NAME, *feat = "Features", *bogusp = "Nope";
    DBusMessage *pg = dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                                   "org.freedesktop.DBus.Properties", "Get");
    dbus_message_append_args(pg, DBUS_TYPE_STRING, &dif, DBUS_TYPE_STRING, &feat, DBUS_TYPE_INVALID);
    r = do_call(&c, names, pg);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    assert(!strcmp(dbus_message_get_signature(r.msg), "v"));
    sdbus_msg_free(&r); dbus_message_unref(pg);

    DBusMessage *pa = dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                                   "org.freedesktop.DBus.Properties", "GetAll");
    dbus_message_append_args(pa, DBUS_TYPE_STRING, &dif, DBUS_TYPE_INVALID);
    r = do_call(&c, names, pa);
    assert(!strcmp(dbus_message_get_signature(r.msg), "a{sv}"));
    { DBusMessageIter it, arr; int n = 0;
      dbus_message_iter_init(r.msg, &it); dbus_message_iter_recurse(&it, &arr);
      while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) { n++; dbus_message_iter_next(&arr); }
      assert(n == 2); }
    sdbus_msg_free(&r); dbus_message_unref(pa);

    DBusMessage *pu = dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                                   "org.freedesktop.DBus.Properties", "Get");
    dbus_message_append_args(pu, DBUS_TYPE_STRING, &dif, DBUS_TYPE_STRING, &bogusp, DBUS_TYPE_INVALID);
    r = do_call(&c, names, pu);
    assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_UNKNOWN_PROPERTY));
    sdbus_msg_free(&r); dbus_message_unref(pu);

    DBusMessage *ps = dbus_message_new_method_call(SDBUS_DRIVER_NAME, SDBUS_DRIVER_PATH,
                                                   "org.freedesktop.DBus.Properties", "Set");
    dbus_message_append_args(ps, DBUS_TYPE_STRING, &dif, DBUS_TYPE_STRING, &feat, DBUS_TYPE_INVALID);
    r = do_call(&c, names, ps);
    assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_PROPERTY_READ_ONLY));
    sdbus_msg_free(&r); dbus_message_unref(ps);

    /* ListQueuedOwners: primary owner first, then the queue */
    { sdbus_transition t[1]; int nt = 0;
      assert(sdbus_names_request(names, c.id, "org.q", 0, t, &nt) == SDBUS_REQ_PRIMARY_OWNER);
      sdbus_names_alloc_unique(names, 2);
      assert(sdbus_names_request(names, 2, "org.q", 0, t, &nt) == SDBUS_REQ_IN_QUEUE); }
    const char *qn = "org.q";
    DBusMessage *lq = mkcall("ListQueuedOwners");
    dbus_message_append_args(lq, DBUS_TYPE_STRING, &qn, DBUS_TYPE_INVALID);
    r = do_call(&c, names, lq);
    { char **v = NULL; int n = 0;
      assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &v, &n, DBUS_TYPE_INVALID));
      assert(n == 2 && !strcmp(v[0], ":1.1") && !strcmp(v[1], ":1.2"));
      dbus_free_string_array(v); }
    sdbus_msg_free(&r); dbus_message_unref(lq);

    const char *nobody = "org.nobody";
    DBusMessage *lqn = mkcall("ListQueuedOwners");
    dbus_message_append_args(lqn, DBUS_TYPE_STRING, &nobody, DBUS_TYPE_INVALID);
    r = do_call(&c, names, lqn);
    assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_NAME_HAS_NO_OWNER));
    sdbus_msg_free(&r); dbus_message_unref(lqn);

    /* ListActivatableNames: driver name + every service-table entry */
    sdbus_svc_ent ents[2] = { { "org.act.one", NULL, NULL }, { "org.act.two", NULL, NULL } };
    sdbus_svctab tab = { ents, 2 };
    g_tab = &tab;
    DBusMessage *lan = mkcall("ListActivatableNames");
    r = do_call(&c, names, lan);
    assert(strv_has(&r, SDBUS_DRIVER_NAME) && strv_has(&r, "org.act.one") && strv_has(&r, "org.act.two"));
    sdbus_msg_free(&r); dbus_message_unref(lan);
    g_tab = NULL;

    /* Ping -> empty return */
    DBusMessage *ping = mkcall("Ping");
    r = do_call(&c, names, ping);
    assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    sdbus_msg_free(&r); dbus_message_unref(ping);

    /* per-conn name cap: fill the registry to the ceiling for conn 1 (cheaply,
       via the names API), then a driver RequestName for a NEW name is rejected
       with LimitsExceeded, while re-requesting an already-held name still works. */
    {
        sdbus_transition tt[1]; int ntt = 0;
        for (int i = 0; i < SDBUS_MAX_NAMES_PER_CONN; i++) {
            char nm[32]; sprintf(nm, "org.cap.n%d", i);
            sdbus_names_request(names, c.id, nm, 0, tt, &ntt);
        }
        DBusMessage *over = mkcall("RequestName");
        const char *newnm = "org.cap.overflow"; dbus_uint32_t f = SDBUS_REQ_DO_NOT_QUEUE;
        dbus_message_append_args(over, DBUS_TYPE_STRING, &newnm, DBUS_TYPE_UINT32, &f, DBUS_TYPE_INVALID);
        r = do_call(&c, names, over);
        assert(dbus_message_get_type(r.msg) == DBUS_MESSAGE_TYPE_ERROR);
        assert(!strcmp(dbus_message_get_error_name(r.msg), DBUS_ERROR_LIMITS_EXCEEDED));
        sdbus_msg_free(&r); dbus_message_unref(over);

        /* re-requesting a name already held bypasses the cap -> ALREADY_OWNER */
        DBusMessage *re = mkcall("RequestName");
        const char *held = "org.cap.n0"; dbus_uint32_t f2 = 0;
        dbus_message_append_args(re, DBUS_TYPE_STRING, &held, DBUS_TYPE_UINT32, &f2, DBUS_TYPE_INVALID);
        r = do_call(&c, names, re);
        dbus_uint32_t rc = 0;
        assert(dbus_message_get_args(r.msg, &e, DBUS_TYPE_UINT32, &rc, DBUS_TYPE_INVALID));
        assert(rc == SDBUS_REQ_ALREADY_OWNER);
        sdbus_msg_free(&r); dbus_message_unref(re);
    }

    /* unknown member -> dispatch returns -1 */
    DBusMessage *bogus = mkcall("NoSuchMethod");
    dbus_message_set_sender(bogus, c.unique);
    dbus_message_set_serial(bogus, 200);
    sdbus_msg bm; memset(&bm, 0, sizeof bm); bm.msg = bogus; bm.member = "NoSuchMethod";
    sdbus_conn *all[] = { &c };
    assert(sdbus_driver_dispatch(&bm, &c, names, all, 1, NULL, record_broadcast, NULL) == -1);
    dbus_message_unref(bogus);

    sdbus_conn_free_fields(&c);
    sdbus_names_free(names);
    printf("all sdbus_driver tests passed\n");
    return 0;
}
