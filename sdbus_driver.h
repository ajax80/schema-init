#ifndef SDBUS_DRIVER_H
#define SDBUS_DRIVER_H

/* The org.freedesktop.DBus driver object. Handles a method_call addressed to
   org.freedesktop.DBus from connection c: mutates the name registry / c's match
   set, marshals a method_return (or error) into c->out, and emits ownership
   transitions through the broadcast callback. Returns 0 if handled, -1 if the
   member is unknown (caller synthesizes UnknownMethod). Service activation
   (StartServiceByName) is a v1.1 stub -> ServiceUnknown. */

#include <dbus/dbus.h>
#include "sdbus_codec.h"
#include "sdbus_names.h"
#include "sdbus_conn.h"
#include <string.h>

#define SDBUS_DRIVER_NAME "org.freedesktop.DBus"
#define SDBUS_DRIVER_PATH "/org/freedesktop/DBus"

typedef void (*sdbus_broadcast_fn)(void *ctx, sdbus_transition *t, int n);

static inline void sdbus__driver_emit(sdbus_conn *c, DBusMessage *reply) {
    static dbus_uint32_t driver_serial;   /* marshalling requires a nonzero serial */
    dbus_message_set_serial(reply, ++driver_serial);
    dbus_message_set_sender(reply, SDBUS_DRIVER_NAME);
    if (c->unique) dbus_message_set_destination(reply, c->unique);
    char *bytes = NULL; int len = 0;
    if (dbus_message_marshal(reply, &bytes, &len)) {
        sdbus_conn_out_append(c, bytes, len);
        dbus_free(bytes);
    }
    dbus_message_unref(reply);
}

static inline sdbus_conn *sdbus__conn_by_id(sdbus_conn **all, int n_all, int id) {
    for (int i = 0; i < n_all; i++) if (all[i]->id == id) return all[i];
    return NULL;
}

/* Resolve a bus name (unique ":1.N" or well-known) to a conn_id, or -1. */
static inline int sdbus__resolve_conn_id(sdbus_names *names, sdbus_conn **all,
                                         int n_all, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < n_all; i++)
        if (all[i]->unique && !strcmp(all[i]->unique, name)) return all[i]->id;
    return sdbus_names_owner(names, name);
}

static inline void sdbus__reply_uint32(sdbus_conn *c, DBusMessage *call, dbus_uint32_t v) {
    DBusMessage *r = dbus_message_new_method_return(call);
    dbus_message_append_args(r, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
    sdbus__driver_emit(c, r);
}
static inline void sdbus__reply_string(sdbus_conn *c, DBusMessage *call, const char *s) {
    DBusMessage *r = dbus_message_new_method_return(call);
    dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
    sdbus__driver_emit(c, r);
}
static inline void sdbus__reply_bool(sdbus_conn *c, DBusMessage *call, int b) {
    dbus_bool_t v = b ? TRUE : FALSE;
    DBusMessage *r = dbus_message_new_method_return(call);
    dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID);
    sdbus__driver_emit(c, r);
}
static inline void sdbus__reply_empty(sdbus_conn *c, DBusMessage *call) {
    sdbus__driver_emit(c, dbus_message_new_method_return(call));
}
static inline void sdbus__reply_error(sdbus_conn *c, DBusMessage *call,
                                      const char *name, const char *msg) {
    sdbus__driver_emit(c, dbus_message_new_error(call, name, msg));
}
static inline void sdbus__reply_strv(sdbus_conn *c, DBusMessage *call,
                                     const char **v, int n) {
    DBusMessage *r = dbus_message_new_method_return(call);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(r, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    for (int i = 0; i < n; i++)
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &v[i]);
    dbus_message_iter_close_container(&it, &arr);
    sdbus__driver_emit(c, r);
}

/* one a{sv} entry whose value is a single uint32 wrapped in a variant */
static inline void sdbus__cred_u32(DBusMessageIter *arr, const char *key, dbus_uint32_t v) {
    DBusMessageIter ent, var;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
    dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "u", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &v);
    dbus_message_iter_close_container(&ent, &var);
    dbus_message_iter_close_container(arr, &ent);
}

/* one a{sv} entry whose value is an array of uint32 (au) wrapped in a variant */
static inline void sdbus__cred_au(DBusMessageIter *arr, const char *key,
                                  const int *ids, int n) {
    DBusMessageIter ent, var, au;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
    dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "au", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "u", &au);
    for (int i = 0; i < n; i++) {
        dbus_uint32_t g = (dbus_uint32_t)ids[i];
        dbus_message_iter_append_basic(&au, DBUS_TYPE_UINT32, &g);
    }
    dbus_message_iter_close_container(&var, &au);
    dbus_message_iter_close_container(&ent, &var);
    dbus_message_iter_close_container(arr, &ent);
}

/* GetConnectionCredentials reply: a{sv} of UnixUserID, ProcessID, UnixGroupIDs
   (the fields SO_PEERCRED + getgrouplist give us; no LinuxSecurityLabel). */
static inline void sdbus__reply_credentials(sdbus_conn *c, DBusMessage *call, sdbus_conn *o) {
    DBusMessage *r = dbus_message_new_method_return(call);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(r, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    sdbus__cred_u32(&arr, "UnixUserID", (dbus_uint32_t)o->uid);
    sdbus__cred_u32(&arr, "ProcessID", (dbus_uint32_t)o->pid);
    if (o->n_gids > 0) sdbus__cred_au(&arr, "UnixGroupIDs", o->gids, o->n_gids);
    dbus_message_iter_close_container(&it, &arr);
    sdbus__driver_emit(c, r);
}

static inline int sdbus_driver_dispatch(sdbus_msg *call, sdbus_conn *c,
        sdbus_names *names, sdbus_conn **all, int n_all,
        sdbus_broadcast_fn broadcast, void *ctx) {
    DBusMessage *m = call->msg;
    const char *member = call->member;
    if (!member) return -1;

    /* org.freedesktop.DBus.Peer */
    if (!strcmp(member, "Ping")) { sdbus__reply_empty(c, m); return 0; }
    if (!strcmp(member, "GetMachineId")) {
        char *id = dbus_get_local_machine_id();
        sdbus__reply_string(c, m, id ? id : "");
        if (id) dbus_free(id);
        return 0;
    }

    if (!strcmp(member, "Hello")) {
        if (!c->unique) c->unique = sdbus_names_alloc_unique(names, c->id);
        c->said_hello = 1;
        sdbus__reply_string(c, m, c->unique);
        return 0;
    }
    if (!strcmp(member, "GetId")) {
        char *id = dbus_get_local_machine_id();
        sdbus__reply_string(c, m, id ? id : "");
        if (id) dbus_free(id);
        return 0;
    }
    if (!strcmp(member, "RequestName")) {
        const char *name = NULL; dbus_uint32_t flags = 0;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "RequestName args");
            return 0;
        }
        /* cap names per connection, but only when this request would add a new
           holding (a re-request of a name we already hold just updates flags) */
        if (!sdbus_names_holds(names, c->id, name) &&
            sdbus_names_count_held(names, c->id) >= SDBUS_MAX_NAMES_PER_CONN) {
            sdbus__reply_error(c, m, DBUS_ERROR_LIMITS_EXCEEDED,
                               "max names per connection exceeded");
            return 0;
        }
        sdbus_transition t[1]; int nt = 0;
        int rc = sdbus_names_request(names, c->id, name, flags, t, &nt);
        if (nt && broadcast) broadcast(ctx, t, nt);
        sdbus__reply_uint32(c, m, (dbus_uint32_t)rc);
        return 0;
    }
    if (!strcmp(member, "ReleaseName")) {
        const char *name = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "ReleaseName args");
            return 0;
        }
        sdbus_transition t[1]; int nt = 0;
        int rc = sdbus_names_release(names, c->id, name, t, &nt);
        if (nt && broadcast) broadcast(ctx, t, nt);
        sdbus__reply_uint32(c, m, (dbus_uint32_t)rc);
        return 0;
    }
    if (!strcmp(member, "NameHasOwner")) {
        const char *name = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "NameHasOwner args");
            return 0;
        }
        int has = !strcmp(name, SDBUS_DRIVER_NAME) ||
                  sdbus__resolve_conn_id(names, all, n_all, name) >= 0;
        sdbus__reply_bool(c, m, has);
        return 0;
    }
    if (!strcmp(member, "GetNameOwner")) {
        const char *name = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "GetNameOwner args");
            return 0;
        }
        if (!strcmp(name, SDBUS_DRIVER_NAME)) { sdbus__reply_string(c, m, SDBUS_DRIVER_NAME); return 0; }
        int oid = sdbus__resolve_conn_id(names, all, n_all, name);
        if (oid < 0) { sdbus__reply_error(c, m, DBUS_ERROR_NAME_HAS_NO_OWNER, "no owner"); return 0; }
        sdbus_conn *o = sdbus__conn_by_id(all, n_all, oid);
        sdbus__reply_string(c, m, o && o->unique ? o->unique : name);
        return 0;
    }
    if (!strcmp(member, "ListNames") || !strcmp(member, "ListActivatableNames")) {
        const char **v = NULL; int n = 0;
        const char *self = SDBUS_DRIVER_NAME;
        v = realloc(v, (n + 1) * sizeof *v); v[n++] = self;
        if (!strcmp(member, "ListNames")) {
            for (int i = 0; i < n_all; i++)
                if (all[i]->unique) { v = realloc(v, (n + 1) * sizeof *v); v[n++] = all[i]->unique; }
            const char **owned; int oc = sdbus_names_list(names, &owned);
            for (int i = 0; i < oc; i++) { v = realloc(v, (n + 1) * sizeof *v); v[n++] = owned[i]; }
            free(owned);
        }
        sdbus__reply_strv(c, m, v, n);
        free(v);
        return 0;
    }
    if (!strcmp(member, "AddMatch")) {
        const char *rule = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "AddMatch args");
            return 0;
        }
        if (!c->matches) c->matches = sdbus_match_new();
        if (sdbus_match_add(c->matches, rule) != 0) {
            sdbus__reply_error(c, m, DBUS_ERROR_MATCH_RULE_INVALID, "invalid match rule");
            return 0;
        }
        sdbus__reply_empty(c, m);
        return 0;
    }
    if (!strcmp(member, "RemoveMatch")) {
        const char *rule = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "RemoveMatch args");
            return 0;
        }
        if (!c->matches || sdbus_match_remove(c->matches, rule) != 0)
            sdbus__reply_error(c, m, DBUS_ERROR_MATCH_RULE_NOT_FOUND, "no such match rule");
        else
            sdbus__reply_empty(c, m);
        return 0;
    }
    if (!strcmp(member, "GetConnectionUnixUser") ||
        !strcmp(member, "GetConnectionUnixProcessID")) {
        const char *name = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "args");
            return 0;
        }
        int oid = sdbus__resolve_conn_id(names, all, n_all, name);
        sdbus_conn *o = oid >= 0 ? sdbus__conn_by_id(all, n_all, oid) : NULL;
        if (!o) { sdbus__reply_error(c, m, DBUS_ERROR_NAME_HAS_NO_OWNER, "no owner"); return 0; }
        dbus_uint32_t val = !strcmp(member, "GetConnectionUnixUser")
                          ? (dbus_uint32_t)o->uid : (dbus_uint32_t)o->pid;
        sdbus__reply_uint32(c, m, val);
        return 0;
    }
    if (!strcmp(member, "GetConnectionCredentials")) {
        const char *name = NULL;
        DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            dbus_error_free(&e);
            sdbus__reply_error(c, m, DBUS_ERROR_INVALID_ARGS, "args");
            return 0;
        }
        int oid = sdbus__resolve_conn_id(names, all, n_all, name);
        sdbus_conn *o = oid >= 0 ? sdbus__conn_by_id(all, n_all, oid) : NULL;
        if (!o) { sdbus__reply_error(c, m, DBUS_ERROR_NAME_HAS_NO_OWNER, "no owner"); return 0; }
        sdbus__reply_credentials(c, m, o);
        return 0;
    }
    if (!strcmp(member, "StartServiceByName")) {   /* v1.1 */
        sdbus__reply_error(c, m, DBUS_ERROR_SERVICE_UNKNOWN,
                           "service activation deferred to schema-dbus v1.1");
        return 0;
    }
    return -1;   /* unknown member */
}

#endif
