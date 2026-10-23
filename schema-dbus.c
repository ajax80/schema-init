/* schema-dbus: a wire-compatible D-Bus system-bus broker. libdbus is linked as
   a wire codec only; the event loop, connection lifecycle, auth, name registry,
   routing, reply-tracking and policy are all ours. Service activation is a v1.1
   stub. See docs/superpowers/specs/2026-09-02-schema-dbus-sp1-broker-design.md */

#include <dbus/dbus.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>

#include "sdbus_codec.h"
#include "sdbus_wire.h"
#include "sdbus_policy.h"
#include "sdbus_names.h"
#include "sdbus_match.h"
#include "sdbus_reply.h"
#include "sdbus_conn.h"
#include "sdbus_auth.h"
#include "sdbus_driver.h"
#include "sdbus_route.h"
#include "sdbus_activate.h"
#include "sdbus_state.h"
#include "sdbus_reload.h"

#define DEFAULT_SOCKET "/run/dbus/system_bus_socket"
#define MAX_TARGETS 256

static sdbus_names   *g_names;
static sdbus_replies *g_replies;
static sdbus_policy  *g_policy;
static sdbus_conn   **g_conns;
static int            g_nconns;
static int            g_epfd;
static unsigned       g_next_id = 1;
static dbus_uint32_t  g_bcast_serial;
static sdbus_svctab  *g_svctab;
static sdbus_acts    *g_acts;
#define SDBUS_MAX_SVCDIRS 8
static const char    *g_svcdirs[SDBUS_MAX_SVCDIRS];
static int            g_nsvcdirs;
static const char    *g_maskfile;
static const char    *g_default_user;
static char         **g_actenv;       /* session bus: env handed to activated services */
static int            g_nmonitors;    /* conns that called BecomeMonitor */
static char           g_bus_addr[256];
static int            g_system_bus;   /* Decision 1, SP4 design doc: --system present
                                          vs absent is the single mode signal gating
                                          session-bus-specific behavior in this file. */
static int            g_draining;     /* SIGTERM: flushing for a planned restart, no more reads */
static char          *g_notify_sock;  /* NOTIFY_SOCKET: PID 1 keeps our fds across a restart */
#define SDBUS_SVC_DIRS "/usr/local/share/dbus-1/system-services:/usr/share/dbus-1/system-services"
#define SDBUS_MASK_FILE "/etc/schema-dbus/masked"
#define SDBUS_SPAWN_TIMEOUT_MS 25000

static pid_t spawn_service(const sdbus_svc_ent *e, const char *bus_addr);
static void reload_request(const char *trigger, int conn_id, uint32_t serial);

/* ---- epoll registration ---- */
static void ep_update(sdbus_conn *c) {
    struct epoll_event ev = {0};
    ev.events = (g_draining ? 0 : EPOLLIN) | (sdbus_conn_has_out(c) ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void ep_add(sdbus_conn *c) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, c->fd, &ev);
}

static sdbus_conn *conn_by_id(int id) {
    for (int i = 0; i < g_nconns; i++) if (g_conns[i]->id == id) return g_conns[i];
    return NULL;
}

/* ---- outbound flush (SCM_RIGHTS for fd-bearing chunks) ---- */
static int flush_conn(sdbus_conn *c) {
    while (c->oq_head < c->n_oq) {
        sdbus_outchunk *ch = &c->oq[c->oq_head];
        struct msghdr mh = {0};
        struct iovec iov;
        iov.iov_base = ch->b + ch->off;
        iov.iov_len  = ch->len - ch->off;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        char cbuf[CMSG_SPACE(sizeof(int) * SDBUS_MAX_PENDING_FDS)];
        if (ch->nfds > 0 && ch->off == 0) {           /* fds go with the first bytes */
            memset(cbuf, 0, sizeof cbuf);
            mh.msg_control = cbuf;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * ch->nfds);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * ch->nfds);
            memcpy(CMSG_DATA(cm), ch->fds, sizeof(int) * ch->nfds);
        }
        ssize_t n = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;                                 /* broken pipe etc. */
        }
        if (ch->off == 0) {                            /* fds have now been sent */
            for (int i = 0; i < ch->nfds; i++) close(ch->fds[i]);
            ch->nfds = 0;                              /* don't re-close on teardown */
        }
        ch->off += (int)n;
        c->oq_bytes -= (int)n;
        if (ch->off >= ch->len) { free(ch->b); c->oq_bytes -= (long)sizeof *ch; c->oq_head++; }
        else return 0;                                 /* partial; wait for writable */
    }
    /* queue drained; compact */
    c->n_oq = c->oq_head = c->oq_cap = 0;
    c->oq_bytes = 0;
    free(c->oq); c->oq = NULL;
    return 0;
}

/* ---- BecomeMonitor: copy traffic to monitor connections ---- */
static const char *wire_type_name(int t) {
    switch (t) {
    case SDBUS_TYPE_METHOD_CALL:   return "method_call";
    case SDBUS_TYPE_METHOD_RETURN: return "method_return";
    case SDBUS_TYPE_ERROR:         return "error";
    case SDBUS_TYPE_SIGNAL:        return "signal";
    }
    return NULL;
}

/* b/len holds one or more complete marshalled messages; fds belong to the first
   and are duplicated, never consumed. skip_id is a conn that must not get a copy
   (the monitor's own traffic). */
static void monitor_copy(const unsigned char *b, int len, const int *fds, int nfds, int skip_id) {
    if (!g_nmonitors) return;
    int off = 0;
    while (off < len) {
        sdbus_wire_msg w;
        int taken = sdbus_wire_parse(b + off, len - off, &w);
        if (taken <= 0) return;
        int mfds = off == 0 ? nfds : 0;
        const char **sown = NULL, **down = NULL; int nsown = 0, ndown = 0;
        const char *dest = w.destination;
        int sid = sdbus__resolve_conn_id(g_names, g_conns, g_nconns, w.sender);
        int did = sdbus__resolve_conn_id(g_names, g_conns, g_nconns, w.destination);
        if (sid >= 0) nsown = sdbus_names_owned_by(g_names, sid, &sown);
        if (did >= 0) {
            ndown = sdbus_names_owned_by(g_names, did, &down);
            const char *du = sdbus_names_unique(g_names, did);
            if (du) dest = du;
        }
        for (int i = 0; i < g_nconns; i++) {
            sdbus_conn *m = g_conns[i];
            if (!m->is_monitor || m->id == skip_id) continue;
            if (mfds && !m->negotiated_fd) continue;
            if (!sdbus_match_message(m->mon_matches, wire_type_name(w.type), w.interface,
                                     w.member, w.path, w.arg0,
                                     w.sender, sown, nsown, dest, down, ndown))
                continue;
            int dup[SDBUS_MAX_PENDING_FDS], nd = 0;
            for (int k = 0; k < mfds && k < SDBUS_MAX_PENDING_FDS; k++) {
                int fd = fcntl(fds[k], F_DUPFD_CLOEXEC, 0);
                if (fd >= 0) dup[nd++] = fd;
            }
            sdbus_conn_enqueue(m, b + off, taken, dup, nd);
            ep_update(m);
        }
        free(sown); free(down);
        off += taken;
    }
}

/* ---- signal broadcast for name transitions ---- */
static void enqueue_signal(sdbus_conn *dst, DBusMessage *sig) {
    dbus_message_set_serial(sig, ++g_bcast_serial);
    dbus_message_set_sender(sig, SDBUS_DRIVER_NAME);
    if (dst->unique) dbus_message_set_destination(sig, dst->unique);
    char *b = NULL; int len = 0;
    if (dbus_message_marshal(sig, &b, &len)) {
        sdbus_conn_enqueue(dst, (unsigned char *)b, len, NULL, 0);
        if (strcmp(dbus_message_get_member(sig), "NameOwnerChanged"))
            monitor_copy((unsigned char *)b, len, NULL, 0, -1);
        dbus_free(b);
        ep_update(dst);
    }
}

/* one broadcast NameOwnerChanged for monitors (listeners get per-destination copies) */
static void monitor_name_owner_changed(const char *name, const char *oldo, const char *newo) {
    if (!g_nmonitors) return;
    DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, "NameOwnerChanged");
    dbus_message_append_args(s, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldo,
                             DBUS_TYPE_STRING, &newo, DBUS_TYPE_INVALID);
    dbus_message_set_serial(s, ++g_bcast_serial);
    dbus_message_set_sender(s, SDBUS_DRIVER_NAME);
    char *b = NULL; int len = 0;
    if (dbus_message_marshal(s, &b, &len)) { monitor_copy((unsigned char *)b, len, NULL, 0, -1); dbus_free(b); }
    dbus_message_unref(s);
}

static const char *unique_or_empty(int conn_id) {
    if (conn_id < 0) return "";
    const char *u = sdbus_names_unique(g_names, conn_id);
    return u ? u : "";
}

/* a name just got an owner: deliver every message held for it. IMPLICIT ones are
   re-routed as a fresh send (records a pending reply so the answer routes back);
   EXPLICIT StartServiceByName callers get a SUCCESS reply. */
static void release_activation(const char *name) {
    sdbus_held_msg *held = NULL; int n = 0;
    if (!sdbus_acts_take(g_acts, name, &held, &n)) return;
    for (int i = 0; i < n; i++) {
        sdbus_conn *caller = conn_by_id(held[i].caller_id);
        if (held[i].kind == SDBUS_HELD_EXPLICIT) {
            if (caller) {
                DBusMessage *r = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
                dbus_message_set_reply_serial(r, held[i].serial);
                dbus_message_set_serial(r, ++g_bcast_serial);
                dbus_message_set_sender(r, SDBUS_DRIVER_NAME);
                if (caller->unique) dbus_message_set_destination(r, caller->unique);
                dbus_uint32_t code = DBUS_START_REPLY_SUCCESS;
                dbus_message_append_args(r, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID);
                char *b = NULL; int len = 0;
                if (dbus_message_marshal(r, &b, &len)) { sdbus_conn_enqueue(caller, (unsigned char *)b, len, NULL, 0); dbus_free(b); ep_update(caller); }
                dbus_message_unref(r);
            }
            for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
            free(held[i].bytes); free(held[i].fds);
            continue;
        }
        /* IMPLICIT: re-route the captured (already sender-stamped) message. */
        sdbus_wire_msg w2;
        if (sdbus_wire_parse(held[i].bytes, held[i].len, &w2) == held[i].len && caller) {
            int targets[MAX_TARGETS], synth2 = 0, denied2 = 0;
            int nt = sdbus_route_targets(&w2, caller, g_names, g_conns, g_nconns,
                                         g_policy, g_replies, &synth2, &denied2, targets, MAX_TARGETS);
            for (int k = 0; k < nt; k++) {
                sdbus_conn *dst = conn_by_id(targets[k]);
                if (!dst) continue;
                if (k == 0 && held[i].nfds > 0) sdbus_conn_enqueue(dst, held[i].bytes, held[i].len, held[i].fds, held[i].nfds);
                else                            sdbus_conn_enqueue(dst, held[i].bytes, held[i].len, NULL, 0);
                ep_update(dst);
            }
            if (!(held[i].nfds > 0 && nt > 0)) for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        } else {
            for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        }
        free(held[i].bytes); free(held[i].fds);
    }
    free(held);
}

/* Tell PID 1 a well-known system-bus name now has an owner, and which pid
   holds it: a service with ready_bus_name= promotes on this (the bus-name
   readiness of systemd Type=dbus). Best-effort; PID 1 may not listen. */
static void report_name_to_pid1(const char *name, pid_t owner_pid) {
    static int fd = -1;
    if (!g_system_bus || name[0] == ':' || owner_pid <= 0) return;
    if (fd < 0) fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", "/run/schema-init/notify");
    char msg[320];
    int len = snprintf(msg, sizeof msg, "BUSNAME=%s\nBUSOWNERPID=%d", name, (int)owner_pid);
    if (len > 0 && (size_t)len < sizeof msg)
        (void)sendto(fd, msg, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a);
}

/* --watch-names: a client of whatever system bus is running (stock
   dbus-daemon included) that reports well-known name owners to PID 1 the
   way the broker does, so ready_bus_name= works without the broker. */
static void watch_report(DBusConnection *c, const char *name) {
    DBusMessage *q, *r;
    dbus_uint32_t pid = 0;
    if (name[0] == ':') return;
    q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    dbus_message_append_args(q, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    r = dbus_connection_send_with_reply_and_block(c, q, 5000, NULL);
    dbus_message_unref(q);
    if (!r) return;
    if (dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID))
        report_name_to_pid1(name, (pid_t)pid);
    dbus_message_unref(r);
}

static int watch_names(void) {
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
    DBusMessage *q, *r, *m;
    DBusMessageIter it, arr;
    if (!c) { fprintf(stderr, "schema-dbus: cannot connect to the system bus\n"); return 1; }
    dbus_connection_set_exit_on_disconnect(c, TRUE);
    dbus_bus_add_match(c, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                          "member='NameOwnerChanged'", NULL);
    q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "ListNames");
    r = dbus_connection_send_with_reply_and_block(c, q, 5000, NULL);
    dbus_message_unref(q);
    if (r && dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        for (dbus_message_iter_recurse(&it, &arr); dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING;
             dbus_message_iter_next(&arr)) {
            const char *name;
            dbus_message_iter_get_basic(&arr, &name);
            watch_report(c, name);
        }
    }
    if (r) dbus_message_unref(r);
    while (dbus_connection_read_write(c, -1)) {
        while ((m = dbus_connection_pop_message(c))) {
            const char *name, *oldo, *newo;
            if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")
                && dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldo,
                                         DBUS_TYPE_STRING, &newo, DBUS_TYPE_INVALID)
                && newo[0])
                watch_report(c, name);
            dbus_message_unref(m);
        }
    }
    return 1;
}

static void broadcast_transitions(void *ctx, sdbus_transition *t, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) {
        const char *name = t[i].name;
        const char *oldo = unique_or_empty(t[i].old_owner);
        const char *newo = unique_or_empty(t[i].new_owner);
        monitor_name_owner_changed(name, oldo, newo);

        /* NameOwnerChanged -> every conn whose match set accepts it */
        for (int j = 0; j < g_nconns; j++) {
            sdbus_conn *cc = g_conns[j];
            if (!cc->matches) continue;
            if (!sdbus_match_signal(cc->matches, SDBUS_DRIVER_NAME, "NameOwnerChanged",
                                    SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, NULL, 0, name))
                continue;
            DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME,
                                                     "NameOwnerChanged");
            dbus_message_append_args(s, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldo,
                                     DBUS_TYPE_STRING, &newo, DBUS_TYPE_INVALID);
            enqueue_signal(cc, s);
            dbus_message_unref(s);
        }
        /* directed NameLost / NameAcquired */
        if (t[i].old_owner >= 0) {
            sdbus_conn *o = conn_by_id(t[i].old_owner);
            if (o) {
                DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, "NameLost");
                dbus_message_append_args(s, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
                enqueue_signal(o, s); dbus_message_unref(s);
            }
        }
        if (t[i].new_owner >= 0) {
            sdbus_conn *o = conn_by_id(t[i].new_owner);
            if (o) {
                DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, "NameAcquired");
                dbus_message_append_args(s, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
                enqueue_signal(o, s); dbus_message_unref(s);
                report_name_to_pid1(name, o->pid);
            }
            release_activation(name);          /* deliver anything held for this name */
        }
    }
}

/* A connection's unique name disappears when it drops, exactly like a well-known
   name losing its last owner. sdbus_names_disconnect only emits transitions for
   well-known names, so a client owning none (e.g. a StatusNotifierItem, which
   registers under its ":1.N") vanished with no NameOwnerChanged at all -- and
   arg0=':1.N' watchers (QDBusServiceWatcher, what KDE's StatusNotifierWatcher
   uses to notice a tray item's client exit) never fired, leaving immortal tray
   icons. Emit NameOwnerChanged(:1.N, :1.N, "") for them. dying_id is skipped: the
   conn is mid-teardown and its fd is about to close. */
static void broadcast_unique_gone(const char *uniq, int dying_id) {
    const char *empty = "";
    monitor_name_owner_changed(uniq, uniq, empty);
    for (int j = 0; j < g_nconns; j++) {
        sdbus_conn *cc = g_conns[j];
        if (cc->id == dying_id || !cc->matches) continue;
        if (!sdbus_match_signal(cc->matches, SDBUS_DRIVER_NAME, "NameOwnerChanged",
                                SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, NULL, 0, uniq))
            continue;
        DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME,
                                                 "NameOwnerChanged");
        dbus_message_append_args(s, DBUS_TYPE_STRING, &uniq, DBUS_TYPE_STRING, &uniq,
                                 DBUS_TYPE_STRING, &empty, DBUS_TYPE_INVALID);
        enqueue_signal(cc, s);
        dbus_message_unref(s);
    }
}

/* move c->out (driver/auth scratch bytes) into the ordered queue as one chunk */
static void drain_scratch(sdbus_conn *c) {
    if (c->out_len > 0) {
        if (c->said_hello && !c->is_monitor) monitor_copy(c->out, c->out_len, NULL, 0, -1);
        sdbus_conn_enqueue(c, c->out, c->out_len, NULL, 0);
        c->out_len = 0;
    }
}

/* build an error reply from scratch (no original DBusMessage needed, so it works
   even for offending messages that carry fds). */
static void synth_error_wire(sdbus_conn *c, uint32_t reply_serial,
                             const char *name, const char *text) {
    DBusMessage *err = dbus_message_new(DBUS_MESSAGE_TYPE_ERROR);
    dbus_message_set_error_name(err, name);
    dbus_message_set_reply_serial(err, reply_serial);
    dbus_message_set_serial(err, ++g_bcast_serial);
    dbus_message_set_sender(err, SDBUS_DRIVER_NAME);
    if (c->unique) dbus_message_set_destination(err, c->unique);
    dbus_message_append_args(err, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    char *b = NULL; int len = 0;
    if (dbus_message_marshal(err, &b, &len)) {
        sdbus_conn_enqueue(c, (unsigned char *)b, len, NULL, 0);
        if (!c->is_monitor) monitor_copy((unsigned char *)b, len, NULL, 0, -1);
        dbus_free(b);
    }
    dbus_message_unref(err);
}

/* deliver a NoReply error to a caller (by conn id) awaiting reply_serial, if it
   is still connected. Used both when a callee disconnects and when a pending call
   times out. */
static void send_no_reply_to(int caller_id, uint32_t reply_serial, const char *text) {
    sdbus_conn *caller = conn_by_id(caller_id);
    if (!caller) return;
    synth_error_wire(caller, reply_serial, DBUS_ERROR_NO_REPLY, text);
    ep_update(caller);
}

/* reply an error to each held caller of an activation that failed, and free the
   held array (bytes/fds owned by the array). */
static void fail_held(sdbus_held_msg *held, int n, const char *errname, const char *text) {
    for (int i = 0; i < n; i++) {
        sdbus_conn *caller = conn_by_id(held[i].caller_id);
        if (caller && held[i].expects_reply) {
            synth_error_wire(caller, held[i].serial, errname, text);
            ep_update(caller);
        }
        for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        free(held[i].bytes); free(held[i].fds);
    }
    free(held);
}

/* drop this message's fds from the head of pending_fds; close them unless they
   were transferred to an outbound chunk (which will close them after sending). */
static void consume_msg_fds(sdbus_conn *c, int nfds, int transferred) {
    if (nfds < 0) nfds = 0;
    if (nfds > c->n_pending_fds) nfds = c->n_pending_fds;   /* never index past what we hold */
    if (!transferred) for (int i = 0; i < nfds; i++) close(c->pending_fds[i]);
    for (int i = nfds; i < c->n_pending_fds; i++) c->pending_fds[i - nfds] = c->pending_fds[i];
    c->n_pending_fds -= nfds;
}

/* If w->destination is an activatable name with no current owner, capture this
   message (reforwarded bytes + dup'd fds + caller + serial), spawn the service if
   not already spawning, and return 1 (message is held, not answered). Return 0 if
   the name is not activatable (caller emits ServiceUnknown as before). The dup'd
   fds are owned by the held copy; the caller still consumes the originals. */
static int activate_or_hold(sdbus_conn *c, sdbus_wire_msg *w,
                            const unsigned char *raw, int rawlen, int nfds) {
    (void)rawlen;
    if (!w->destination) return 0;
    const sdbus_svc_ent *e = sdbus_svctab_find(g_svctab, w->destination);
    if (!e) return 0;

    /* capture the reforwarded (sender-stamped) bytes so release re-injects a
       ready-to-route message. */
    unsigned char *fwd = NULL; int fwlen = 0;
    if (sdbus_wire_reforward(raw, w, c->unique, &fwd, &fwlen) != 0) return 0;

    sdbus_held_msg m = {0};
    m.bytes = fwd; m.len = fwlen;
    m.caller_id = c->id; m.serial = w->serial;
    m.expects_reply = !(w->flags & SDBUS_FLAG_NO_REPLY);
    m.kind = SDBUS_HELD_IMPLICIT;
    if (nfds > 0) {
        m.fds = malloc(nfds * sizeof(int));
        for (int i = 0; i < nfds; i++) m.fds[i] = dup(c->pending_fds[i]);
        m.nfds = nfds;
    }

    sdbus_pending_act *pa = sdbus_acts_find(g_acts, w->destination);
    if (!pa) {
        pid_t pid = spawn_service(e, g_bus_addr);
        if (pid < 0) {                          /* fork failed -> ServiceUnknown path */
            free(fwd);
            for (int i = 0; i < m.nfds; i++) close(m.fds[i]);
            free(m.fds);
            return 0;
        }
        pa = sdbus_acts_begin(g_acts, w->destination, pid,
                              sdbus__now_ms() + SDBUS_SPAWN_TIMEOUT_MS);
    }
    sdbus_acts_hold(pa, &m);        /* deep-copies bytes + fds */
    free(fwd);
    for (int i = 0; i < m.nfds; i++) close(m.fds[i]);
    free(m.fds);
    return 1;
}

/* ---- process one fully-buffered message (parsed header w, raw bytes) ---- */
static void handle_message(sdbus_conn *c, sdbus_wire_msg *w, const unsigned char *raw, int rawlen) {
    int nfds = sdbus_wire_n_fds(w);   /* this message's fds sit at head of pending_fds */

    /* the fd count is attacker-controlled (UNIX_FDS header field); a message
       must not claim more fds than were actually passed, or downstream code
       would touch pending_fds[] entries we never received. */
    if (nfds < 0 || nfds > c->n_pending_fds) {
        synth_error_wire(c, w->serial, DBUS_ERROR_INVALID_ARGS, "unix_fds count mismatch");
        for (int i = 0; i < c->n_pending_fds; i++) close(c->pending_fds[i]);
        c->n_pending_fds = 0;
        drain_scratch(c); ep_update(c);
        return;
    }

    if (c->is_monitor) {   /* monitors are receive-only: drop anything they send */
        consume_msg_fds(c, nfds, 0);
        return;
    }
    if (g_nmonitors) {
        unsigned char *mb = NULL; int ml = 0;
        if (c->unique && sdbus_wire_reforward(raw, w, c->unique, &mb, &ml) == 0) {
            monitor_copy(mb, ml, c->pending_fds, nfds, c->id);
            free(mb);
        } else {
            monitor_copy(raw, rawlen, c->pending_fds, nfds, c->id);
        }
    }

    int to_driver = w->destination && !strcmp(w->destination, SDBUS_DRIVER_NAME);
    if (!w->destination && w->interface && !strcmp(w->interface, "org.freedesktop.DBus.Peer"))
        to_driver = 1;   /* Peer methods (Ping/GetMachineId) may omit the destination */

    if (to_driver && w->member && !strcmp(w->member, "StartServiceByName")) {
        /* explicit activation: arg0 = target name, arg1 = flags (su) */
        sdbus_msg dm;
        const char *target = NULL;
        dbus_uint32_t sflags = 0;
        int parsed = (sdbus_codec_take(raw, rawlen, &dm) == rawlen);
        if (parsed) {
            DBusError e; dbus_error_init(&e);
            if (!dbus_message_get_args(dm.msg, &e, DBUS_TYPE_STRING, &target,
                                       DBUS_TYPE_UINT32, &sflags, DBUS_TYPE_INVALID)) {
                dbus_error_free(&e); target = NULL;
            }
        }
        if (!target) {
            synth_error_wire(c, w->serial, DBUS_ERROR_INVALID_ARGS, "StartServiceByName needs a name");
        } else if (sdbus_names_owner(g_names, target) >= 0) {
            DBusMessage *r = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
            dbus_message_set_reply_serial(r, w->serial);
            dbus_message_set_serial(r, ++g_bcast_serial);
            dbus_message_set_sender(r, SDBUS_DRIVER_NAME);
            if (c->unique) dbus_message_set_destination(r, c->unique);
            dbus_uint32_t code = DBUS_START_REPLY_ALREADY_RUNNING;
            dbus_message_append_args(r, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID);
            char *b = NULL; int len = 0;
            if (dbus_message_marshal(r, &b, &len)) { sdbus_conn_enqueue(c, (unsigned char *)b, len, NULL, 0); dbus_free(b); }
            dbus_message_unref(r);
        } else {
            const sdbus_svc_ent *ent = sdbus_svctab_find(g_svctab, target);
            if (!ent) {
                synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "no such activatable service");
            } else {
                sdbus_pending_act *pa = sdbus_acts_find(g_acts, target);
                if (!pa) {
                    pid_t pid = spawn_service(ent, g_bus_addr);
                    if (pid < 0) synth_error_wire(c, w->serial, DBUS_ERROR_SPAWN_FAILED, "fork failed");
                    else pa = sdbus_acts_begin(g_acts, target, pid, sdbus__now_ms() + SDBUS_SPAWN_TIMEOUT_MS);
                }
                if (pa) {
                    sdbus_held_msg m = {0};
                    m.caller_id = c->id; m.serial = w->serial; m.expects_reply = 1;
                    m.kind = SDBUS_HELD_EXPLICIT;
                    sdbus_acts_hold(pa, &m);
                }
            }
        }
        if (parsed) sdbus_msg_free(&dm);
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }

    if (to_driver && w->member && !strcmp(w->member, "BecomeMonitor")) {
        sdbus_msg dm;
        int parsed = (sdbus_codec_take(raw, rawlen, &dm) == rawlen);
        sdbus_matchset *rules = NULL;
        if (!parsed) {
            synth_error_wire(c, w->serial, DBUS_ERROR_FAILED, "cannot parse message");
        } else if (!(c->uid == 0 || c->uid == getuid())) {
            sdbus__reply_error(c, dm.msg, DBUS_ERROR_ACCESS_DENIED, "BecomeMonitor not permitted");
        } else {
            char **rv = NULL; int nr = 0; dbus_uint32_t flags = 1;
            DBusError e; dbus_error_init(&e);
            int ok = dbus_message_get_args(dm.msg, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &rv, &nr,
                                           DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID);
            dbus_error_free(&e);
            if (!ok || flags != 0) {
                sdbus__reply_error(c, dm.msg, DBUS_ERROR_INVALID_ARGS, "BecomeMonitor expects (as rules, u 0)");
            } else {
                if (nr > SDBUS_MAX_MATCH_RULES) {
                    sdbus__reply_error(c, dm.msg, DBUS_ERROR_LIMITS_EXCEEDED, "too many match rules");
                } else {
                    rules = sdbus_match_new();
                    for (int i = 0; i < nr && rules; i++)
                        if (sdbus_match_add(rules, rv[i]) != 0) { sdbus_match_free(rules); rules = NULL; }
                    if (!rules) sdbus__reply_error(c, dm.msg, DBUS_ERROR_MATCH_RULE_INVALID, "invalid match rule");
                    else        sdbus__reply_empty(c, dm.msg);
                }
            }
            if (rv) dbus_free_string_array(rv);
        }
        if (parsed) sdbus_msg_free(&dm);
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        if (rules) {
            /* the monitor gives up its names and stops being addressable; NameLost
               for its own unique name is how clients (busctl) learn it took effect */
            char *uniq = c->unique ? strdup(c->unique) : NULL;
            if (uniq) {
                DBusMessage *nl = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, "NameLost");
                const char *un = uniq;
                dbus_message_append_args(nl, DBUS_TYPE_STRING, &un, DBUS_TYPE_INVALID);
                enqueue_signal(c, nl);
                dbus_message_unref(nl);
            }
            int tcap = g_names->n_names; int nt = 0;
            sdbus_transition *t = tcap > 0 ? malloc(tcap * sizeof *t) : NULL;
            sdbus_names_disconnect(g_names, c->id, t, &nt, tcap);
            c->unique = NULL;
            if (nt) broadcast_transitions(NULL, t, nt);
            free(t);
            if (uniq) { broadcast_unique_gone(uniq, c->id); free(uniq); }
            if (c->matches) { sdbus_match_free(c->matches); c->matches = NULL; }
            c->mon_matches = rules;
            c->is_monitor = 1;
            g_nmonitors++;
        }
        return;
    }

    if (to_driver && w->member &&
        (!strcmp(w->member, "ReloadConfig") || !strcmp(w->member, "UpdateActivationEnvironment"))) {
        sdbus_msg dm;
        int parsed = (sdbus_codec_take(raw, rawlen, &dm) == rawlen);
        int privileged = c->uid == 0 || (uid_t)c->uid == getuid();
        if (!parsed) {
            synth_error_wire(c, w->serial, DBUS_ERROR_FAILED, "cannot parse message");
        } else if (!strcmp(w->member, "ReloadConfig")) {
            if (!privileged) {
                sdbus__reply_error(c, dm.msg, DBUS_ERROR_ACCESS_DENIED, "ReloadConfig not permitted");
            } else if (g_draining) {   /* restarting: the next broker reads the new config */
                synth_error_wire(c, w->serial, DBUS_ERROR_FAILED, "Message bus is restarting");
            } else {
                reload_request("ReloadConfig", c->id, w->serial);
            }
        } else if (g_system_bus) {
            sdbus__reply_error(c, dm.msg, DBUS_ERROR_ACCESS_DENIED,
                               "Cannot change activation environment on a system bus.");
        } else if (!privileged) {
            sdbus__reply_error(c, dm.msg, DBUS_ERROR_ACCESS_DENIED,
                               "UpdateActivationEnvironment not permitted");
        } else {
            DBusMessageIter it, arr, ent;
            int ok = dbus_message_iter_init(dm.msg, &it) &&
                     !strcmp(dbus_message_get_signature(dm.msg), "a{ss}");
            if (ok) {
                dbus_message_iter_recurse(&it, &arr);
                while (ok && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                    const char *k, *v;
                    dbus_message_iter_recurse(&arr, &ent);
                    dbus_message_iter_get_basic(&ent, &k);
                    dbus_message_iter_next(&ent);
                    dbus_message_iter_get_basic(&ent, &v);
                    if (sdbus_activate_env_set(&g_actenv, k, v) != 0) ok = 0;
                    dbus_message_iter_next(&arr);
                }
            }
            if (ok) sdbus__reply_empty(c, dm.msg);
            else    sdbus__reply_error(c, dm.msg, DBUS_ERROR_INVALID_ARGS, "expected a{ss} of valid names");
        }
        if (parsed) sdbus_msg_free(&dm);
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }

    if (to_driver) {
        /* driver methods never carry fds -> libdbus can demarshal to read args */
        sdbus_msg dm;
        if (sdbus_codec_take(raw, rawlen, &dm) == rawlen) {
            int rc = sdbus_driver_dispatch(&dm, c, g_names, g_conns, g_nconns,
                                           g_svctab, g_policy, broadcast_transitions, NULL);
            if (rc < 0)
                synth_error_wire(c, w->serial, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
            sdbus_msg_free(&dm);
        } else {
            synth_error_wire(c, w->serial, DBUS_ERROR_FAILED, "cannot parse message");
        }
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }

    if (!c->unique) {   /* routing requires a Hello-assigned unique name to stamp */
        synth_error_wire(c, w->serial, DBUS_ERROR_ACCESS_DENIED, "Hello required first");
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }

    /* validate before relaying to any peer: stock dbus-daemon rejects a malformed
       message at the sender, and a client that receives one off the bus disconnects.
       Forwarding garbage would let one sender drop every peer it can reach. */
    if (!sdbus_wire_forwardable(raw, rawlen, w)) {
        synth_error_wire(c, w->serial, DBUS_ERROR_INVALID_ARGS, "malformed message");
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }

    int targets[MAX_TARGETS], synth = 0, denied = 0;
    int nt = sdbus_route_targets(w, c, g_names, g_conns, g_nconns, g_policy,
                                 g_replies, &synth, &denied, targets, MAX_TARGETS);
    int transferred = 0;
    if (denied) {
        synth_error_wire(c, w->serial, DBUS_ERROR_ACCESS_DENIED, "rejected by policy");
    } else if (synth) {
        if (!activate_or_hold(c, w, raw, rawlen, nfds))
            synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "name has no owner");
    } else if (nt > 0) {
        unsigned char *bytes = NULL; int len = 0;
        if (sdbus_wire_reforward(raw, w, c->unique, &bytes, &len) == 0) {
            for (int i = 0; i < nt; i++) {
                sdbus_conn *dst = conn_by_id(targets[i]);
                if (!dst || dst->is_monitor) continue;
                if (i == 0 && nfds > 0) { sdbus_conn_enqueue(dst, bytes, len, c->pending_fds, nfds); transferred = 1; }
                else                    sdbus_conn_enqueue(dst, bytes, len, NULL, 0);
                ep_update(dst);
            }
            free(bytes);
        }
    }
    drain_scratch(c); ep_update(c);
    consume_msg_fds(c, nfds, transferred);
}

static void process_inbound(sdbus_conn *c) {
    for (;;) {
        sdbus_wire_msg w;
        int taken = sdbus_wire_parse(c->in, c->in_len, &w);
        if (taken == 0) return;                    /* need more */
        if (taken < 0) {                           /* corrupt: drop buffer + its fds */
            for (int i = 0; i < c->n_pending_fds; i++) close(c->pending_fds[i]);
            c->n_pending_fds = 0; c->in_len = 0; return;
        }
        handle_message(c, &w, c->in, taken);       /* w points into c->in; used before consume */
        sdbus_conn_in_consume(c, taken);
    }
}

/* ---- connection lifecycle ---- */
static void add_conn(int fd) {
    sdbus_conn *c = calloc(1, sizeof *c);
    c->fd = fd;
    c->id = (int)g_next_id++;
    sdbus_conn_capture_creds(c);
    /* bound how many connections one (non-root) user can hold open, so a single
       local uid cannot exhaust the broker's fds/memory by connecting in a loop.
       root (system daemons) is exempt, as in dbus-daemon. */
    if (c->uid != 0 && c->uid != (uid_t)-1) {
        int same = 0;
        for (int i = 0; i < g_nconns; i++) if (g_conns[i]->uid == c->uid) same++;
        if (same >= SDBUS_MAX_CONN_PER_UID) { free(c); close(fd); return; }
    }
    g_conns = realloc(g_conns, (g_nconns + 1) * sizeof *g_conns);
    g_conns[g_nconns++] = c;
    ep_add(c);
}

static void remove_conn(sdbus_conn *c) {
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    if (c->is_monitor) g_nmonitors--;
    /* copy before disconnect frees it; watchers of the unique name need it below */
    char *uniq = c->unique ? strdup(c->unique) : NULL;
    /* a conn can primary-own up to every name on the bus; size the transition
       buffer to that so RequestName-many-then-disconnect can't overflow it */
    int tcap = g_names->n_names; int nt = 0;
    sdbus_transition *t = tcap > 0 ? malloc(tcap * sizeof *t) : NULL;
    sdbus_names_disconnect(g_names, c->id, t, &nt, tcap);
    c->unique = NULL;   /* names just freed this string; don't let signals deref it */
    if (nt) broadcast_transitions(NULL, t, nt);
    free(t);
    if (uniq) { broadcast_unique_gone(uniq, c->id); free(uniq); }
    /* callee vanished: any caller still awaiting a reply from it would hang until
       its own timeout (many clients set none), so synthesize a NoReply error to
       each stranded caller before the entries are purged. */
    if (g_replies->n > 0) {
        int cap = g_replies->n;
        int *callers = malloc(cap * sizeof *callers);
        uint32_t *serials = malloc(cap * sizeof *serials);
        int np = sdbus_replies_pending_on(g_replies, c->id, c->id, callers, serials, cap);
        for (int i = 0; i < np; i++)
            send_no_reply_to(callers[i], serials[i],
                "Message recipient disconnected from message bus without replying");
        free(callers); free(serials);
    }
    sdbus_replies_purge(g_replies, c->id);
    for (int i = 0; i < c->n_pending_fds; i++) close(c->pending_fds[i]);
    close(c->fd);
    sdbus_conn_free_fields(c);
    for (int i = 0; i < g_nconns; i++)
        if (g_conns[i] == c) { g_conns[i] = g_conns[--g_nconns]; break; }
    free(c);
}

/* returns 1 if the connection survived, 0 if it was removed */
static int on_readable(sdbus_conn *c) {
    unsigned char buf[65536];
    char cbuf[CMSG_SPACE(sizeof(int) * SDBUS_MAX_PENDING_FDS)];
    struct msghdr mh = {0};
    struct iovec iov = { buf, sizeof buf };
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    ssize_t n = recvmsg(c->fd, &mh, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { remove_conn(c); return 0; }
        return 1;
    }

    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int cnt = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *fds = (int *)CMSG_DATA(cm);
            for (int i = 0; i < cnt; i++) {
                if (c->n_pending_fds < SDBUS_MAX_PENDING_FDS) c->pending_fds[c->n_pending_fds++] = fds[i];
                else close(fds[i]);
            }
        }
    }

    if (!c->authed) {
        int r = sdbus_auth_feed(c, buf, (int)n);
        drain_scratch(c);           /* auth responses go out via the queue */
        ep_update(c);
        if (r < 0) { remove_conn(c); return 0; }
        if (r == 1) process_inbound(c);   /* BEGIN reached; remaining bytes are messages */
        return 1;
    }
    sdbus_conn_in_append(c, buf, (int)n);
    process_inbound(c);
    return 1;
}

/* ---- live config reload ----
   SIGHUP, ReloadConfig and (debounced) inotify all land here. A reload rebuilds
   the activation table and, on the system bus, re-dissolves the busconfig in a
   child so the event loop never waits on python3. Nothing is swapped until the
   new policy dissolved, parsed and passed sdbus_reload_policy_ok; any failure
   keeps the old policy and table. Never a restart: connections, names, matches
   and pending replies are untouched. */
typedef struct { int conn_id; uint32_t serial; } reload_waiter;
static struct {
    pid_t pid;                        /* dissolve child, 0 = none running */
    long deadline;
    int running, dirty;
    const char *trigger, *next_trigger;
    reload_waiter *run, *next; int nrun, nnext;
    sdbus_svctab *tab;                /* new table, swapped with the policy */
    struct timespec started;          /* the dissolve's start: the cache's mtime */
} g_rl;
static const char *g_dissect, *g_system_conf, *g_cache, *g_override_dir, *g_polfile;
static long g_reload_timeout_ms = 30000;

static void synth_return_wire(sdbus_conn *c, uint32_t reply_serial) {
    DBusMessage *r = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    dbus_message_set_reply_serial(r, reply_serial);
    dbus_message_set_serial(r, ++g_bcast_serial);
    dbus_message_set_sender(r, SDBUS_DRIVER_NAME);
    if (c->unique) dbus_message_set_destination(r, c->unique);
    char *b = NULL; int len = 0;
    if (dbus_message_marshal(r, &b, &len)) {
        sdbus_conn_enqueue(c, (unsigned char *)b, len, NULL, 0);
        if (!c->is_monitor) monitor_copy((unsigned char *)b, len, NULL, 0, -1);
        dbus_free(b);
    }
    dbus_message_unref(r);
}

static void reload_reply(reload_waiter *v, int n, const char *err) {
    for (int i = 0; i < n; i++) {
        sdbus_conn *c = conn_by_id(v[i].conn_id);
        if (!c) continue;
        if (err) synth_error_wire(c, v[i].serial, DBUS_ERROR_FAILED, err);
        else     synth_return_wire(c, v[i].serial);
        ep_update(c);
    }
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = n >= 0 ? malloc(n + 1) : NULL;
    if (t && fread(t, 1, n, f) != (size_t)n) { free(t); t = NULL; }
    if (t) t[n] = '\0';
    fclose(f);
    return t;
}

static void reload_tmp(char *buf, size_t n) { snprintf(buf, n, "%s.reload.tmp", g_polfile); }

static void reload_start(void);

/* one outcome per reload: policy (if dissolved) and table swap together or not
   at all. pol is the parsed new policy, NULL when this reload dissolved none. */
static void reload_finish(sdbus_policy *pol, const char *err) {
    char why[256];
    if (!err && pol) {
        int n = 0;
        for (int i = 0; i < g_names->n_names; i++) n += g_names->names[i].n_holders > 0;
        sdbus_owner *own = calloc(n + 1, sizeof *own);
        int k = 0;
        for (int i = 0; i < g_names->n_names; i++) {
            sdbus_name_ent *e = &g_names->names[i];
            sdbus_conn *o = e->n_holders > 0 ? conn_by_id(e->holders[0].conn_id) : NULL;
            if (!o) continue;
            own[k++] = (sdbus_owner){ e->name, (int)o->uid, o->gids, o->n_gids };
        }
        if (!sdbus_reload_policy_ok(g_policy, pol, own, k, why, sizeof why)) err = why;
        else if (why[0]) fprintf(stderr, "schema-dbus: reload (%s): %s\n", g_rl.trigger, why);
        free(own);
    }
    char tmp[1100];
    if (g_polfile) reload_tmp(tmp, sizeof tmp);
    if (err) {
        if (pol) sdbus_policy_free(pol);
        sdbus_svctab_free(g_rl.tab);
        if (g_polfile) unlink(tmp);
        fprintf(stderr, "schema-dbus: reload (%s) failed: %s, keeping the old policy\n", g_rl.trigger, err);
    } else {
        if (pol) {
            int nr = sdbus_policy_rule_count(pol);
            sdbus_policy_free(g_policy);
            g_policy = pol;
            if (rename(tmp, g_polfile) != 0)
                fprintf(stderr, "schema-dbus: reload: %s not updated: %s\n", g_polfile, strerror(errno));
            else if (g_cache) {
                /* the launcher's boot cache, so the next boot needn't dissolve again */
                char ctmp[1100];
                snprintf(ctmp, sizeof ctmp, "%s.tmp", g_cache);
                char *t = slurp(g_polfile);
                FILE *f = t ? fopen(ctmp, "w") : NULL;
                /* stamped with the dissolve's start, as the launcher does, so a
                   conf changed during it still looks newer at the next boot */
                struct timespec ts[2] = { g_rl.started, g_rl.started };
                if (f && fputs(t, f) >= 0 && fflush(f) == 0 && futimens(fileno(f), ts) == 0 && fclose(f) == 0)
                    rename(ctmp, g_cache);
                else { if (f) fclose(f); unlink(ctmp); }
                free(t);
            }
            fprintf(stderr, "schema-dbus: reload (%s): policy %d rules, %d activatable services\n",
                    g_rl.trigger, nr, g_rl.tab->n);
        } else {
            fprintf(stderr, "schema-dbus: reload (%s): policy unchanged, %d activatable services\n",
                    g_rl.trigger, g_rl.tab->n);
        }
        sdbus_svctab_free(g_svctab);
        g_svctab = g_rl.tab;
    }
    g_rl.tab = NULL;
    reload_reply(g_rl.run, g_rl.nrun, err);
    free(g_rl.run); g_rl.run = NULL; g_rl.nrun = 0;
    g_rl.running = 0;
    if (g_rl.dirty) {
        g_rl.dirty = 0;
        g_rl.run = g_rl.next; g_rl.nrun = g_rl.nnext;
        g_rl.next = NULL; g_rl.nnext = 0;
        g_rl.trigger = g_rl.next_trigger;
        reload_start();
    }
}

static void reload_start(void) {
    g_rl.running = 1;
    g_rl.tab = sdbus_svctab_parse_dirs_masked(g_svcdirs, g_nsvcdirs, g_maskfile, g_default_user);
    if (g_system_bus) sdbus_svctab_hold_raw(g_rl.tab, g_svctab, g_override_dir);
    if (!g_system_bus || !g_dissect || !g_polfile) { reload_finish(NULL, NULL); return; }
    char tmp[1100];
    reload_tmp(tmp, sizeof tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { reload_finish(NULL, "cannot write the new policy"); return; }
    clock_gettime(CLOCK_REALTIME, &g_rl.started);
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);   /* not the broker's OOM shield */
        if (ofd >= 0) { (void)!write(ofd, "0\n", 2); close(ofd); }
        dup2(fd, 1);
        execlp("python3", "python3", g_dissect, g_system_conf, (char *)NULL);
        _exit(127);
    }
    close(fd);
    if (pid < 0) { reload_finish(NULL, "fork failed"); return; }
    g_rl.pid = pid;
    g_rl.deadline = sdbus__now_ms() + g_reload_timeout_ms;
}

static void reload_request(const char *trigger, int conn_id, uint32_t serial) {
    reload_waiter **v = g_rl.running ? &g_rl.next : &g_rl.run;
    int *n = g_rl.running ? &g_rl.nnext : &g_rl.nrun;
    if (conn_id) {
        *v = realloc(*v, (*n + 1) * sizeof **v);
        (*v)[(*n)++] = (reload_waiter){ conn_id, serial };
    }
    if (g_rl.running) {
        /* covered by one more reload after this one: the files may have changed
           after the running dissolve read them */
        if (!g_rl.dirty || !strcmp(trigger, "ReloadConfig")) g_rl.next_trigger = trigger;
        g_rl.dirty = 1;
        return;
    }
    g_rl.trigger = trigger;
    reload_start();
}

/* SIGCHLD path: 1 if p was the dissolve child */
static int reload_reaped(pid_t p, int st) {
    if (!g_rl.pid || p != g_rl.pid) return 0;
    g_rl.pid = 0;
    char why[64];
    if (!WIFEXITED(st)) { snprintf(why, sizeof why, "dissolver killed by signal %d", WTERMSIG(st)); reload_finish(NULL, why); return 1; }
    if (WEXITSTATUS(st)) { snprintf(why, sizeof why, "dissolver exited %d", WEXITSTATUS(st)); reload_finish(NULL, why); return 1; }
    char tmp[1100];
    reload_tmp(tmp, sizeof tmp);
    char *t = slurp(tmp);
    if (!t || !*t) { free(t); reload_finish(NULL, "dissolver produced no policy"); return 1; }
    sdbus_policy *pol = sdbus_policy_parse(t);
    free(t);
    reload_finish(pol, pol ? NULL : "new policy did not parse");
    return 1;
}

static void reload_kill_child(void) {
    if (!g_rl.pid) return;
    kill(g_rl.pid, SIGKILL);
    waitpid(g_rl.pid, NULL, 0);
    g_rl.pid = 0;
}

static void reload_check_timeout(void) {
    if (g_rl.pid && sdbus__now_ms() >= g_rl.deadline) {
        reload_kill_child();
        reload_finish(NULL, "dissolver timed out");
    }
}

/* SIGTERM: a planned restart must not leave a dissolver behind or a caller
   waiting on a reply the next broker knows nothing about */
static void reload_abort(void) {
    reload_kill_child();
    if (g_polfile && g_rl.running) { char tmp[1100]; reload_tmp(tmp, sizeof tmp); unlink(tmp); }
    reload_reply(g_rl.run, g_rl.nrun, "Message bus is restarting");
    reload_reply(g_rl.next, g_rl.nnext, "Message bus is restarting");
    g_rl.nrun = g_rl.nnext = 0;
}

/* ---- automatic pickup: inotify on the D-Bus config, like dbus-daemon ----
   A dnf transaction drops many files, so any event only pushes a quiet-window
   deadline out; the reload runs once the watched set has been still for it. */
typedef struct { int wd; char *only; } cfg_watch;   /* only: a file name in a watched dir, NULL = any */
static cfg_watch *g_watches;
static int g_nwatches, g_ifd = -1;
static long g_debounce_ms = 1000, g_debounce_at;
/* every dir we mean to watch. One missing (or deleted) is covered by a watch on
   its parent for its name, and watched itself once it appears: the override dir
   may first be created by schema-import mid-dnf, and a held SystemdService=
   entry is released only when inotify sees its override land. */
typedef struct { char *path, *only; int wd; } cfg_want;
static cfg_want *g_wants;
static int g_nwants;

static int watch_add(const char *path, const char *only) {
    int wd = inotify_add_watch(g_ifd, path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                            IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                            IN_ONLYDIR | IN_MASK_ADD);
    if (wd < 0) return -1;
    for (int i = 0; i < g_nwatches; i++)
        if (g_watches[i].wd == wd && (only ? g_watches[i].only && !strcmp(g_watches[i].only, only)
                                           : !g_watches[i].only))
            return wd;
    g_watches = realloc(g_watches, (g_nwatches + 1) * sizeof *g_watches);
    g_watches[g_nwatches++] = (cfg_watch){ wd, only ? strdup(only) : NULL };
    return wd;
}

static void watch_sync(void) {
    for (int i = 0; i < g_nwants; i++) {
        cfg_want *w = &g_wants[i];
        if (w->wd >= 0) continue;
        if ((w->wd = watch_add(w->path, w->only)) >= 0) continue;
        const char *slash = strrchr(w->path, '/');
        if (!slash || slash == w->path) continue;
        char parent[1024];
        snprintf(parent, sizeof parent, "%.*s", (int)(slash - w->path), w->path);
        watch_add(parent, slash + 1);
    }
}

static void watch_want(const char *path, const char *only) {
    g_wants = realloc(g_wants, (g_nwants + 1) * sizeof *g_wants);
    g_wants[g_nwants++] = (cfg_want){ strdup(path), only ? strdup(only) : NULL, -1 };
}

static void watch_file(const char *path) {
    char dir[1024];
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
    watch_want(*dir ? dir : "/", slash + 1);
}

static void watch_setup(void) {
    g_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_ifd < 0) { fprintf(stderr, "schema-dbus: inotify unavailable, config changes need a reload\n"); return; }
    for (int i = 0; i < g_nsvcdirs; i++) watch_want(g_svcdirs[i], NULL);
    if (g_maskfile) watch_file(g_maskfile);
    if (g_system_bus) {
        watch_file(g_system_conf);
        watch_want("/usr/share/dbus-1/system.d", NULL);
        watch_want("/etc/dbus-1", NULL);
        watch_want("/etc/dbus-1/system.d", NULL);
    }
    const char *extra = getenv("SCHEMA_DBUS_WATCH_EXTRA");
    if (extra) {
        char *buf = strdup(extra), *save = NULL;
        for (char *p = strtok_r(buf, ":", &save); p; p = strtok_r(NULL, ":", &save)) watch_want(p, NULL);
        free(buf);
    }
    watch_sync();
}

static void watch_read(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    int hit = 0;
    while ((n = read(g_ifd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { hit = 1; continue; }
            for (int i = 0; i < g_nwatches; i++) {
                if (g_watches[i].wd != ev->wd) continue;
                if (!g_watches[i].only || (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                    || (ev->len && !strcmp(ev->name, g_watches[i].only)))
                    hit = 1;
            }
            if (ev->mask & IN_IGNORED) {     /* dir gone: its watch is dead */
                for (int i = 0; i < g_nwatches; i++)
                    if (g_watches[i].wd == ev->wd) {
                        free(g_watches[i].only);
                        g_watches[i--] = g_watches[--g_nwatches];
                    }
                for (int i = 0; i < g_nwants; i++)
                    if (g_wants[i].wd == ev->wd) g_wants[i].wd = -1;
            }
        }
    }
    if (hit) {
        watch_sync();
        g_debounce_at = sdbus__now_ms() + g_debounce_ms;
    }
}

/* ---- planned restart through the PID 1 fdstore ---- */
#define FDSTORE_BATCH 253   /* the kernel's SCM_MAX_FD: more in one message is refused */

static int notify_fds(const char *msg, const int *fds, int nfds) {
    static int fd = -1;
    if (fd < 0) fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", g_notify_sock);
    struct iovec iov = { (void *)msg, strlen(msg) };
    struct msghdr mh = {0};
    mh.msg_name = &a; mh.msg_namelen = sizeof a;
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int) * FDSTORE_BATCH)];
    if (nfds > 0) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
    }
    return sendmsg(fd, &mh, MSG_NOSIGNAL) < 0 ? -1 : 0;
}

static int store_fds(const char *name, const int *fds, int n) {
    char msg[64];
    snprintf(msg, sizeof msg, "FDSTORE=1\nFDNAME=%s", name);
    for (int off = 0; off < n; off += FDSTORE_BATCH)
        if (notify_fds(msg, fds + off, n - off < FDSTORE_BATCH ? n - off : FDSTORE_BATCH) < 0) return -1;
    return 0;
}

static void store_remove_all(void) {
    static const char *names[] = { "listen", "state", "conn", "pfd" };
    if (!g_notify_sock) return;
    for (int i = 0; i < 4; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "FDSTOREREMOVE=1\nFDNAME=%s", names[i]);
        notify_fds(msg, NULL, 0);
    }
}

/* SIGTERM: stop accepting and reading, run what is already buffered, flush the
   outbound queues for up to 2 s and drop whoever is still behind. Then, under
   PID 1, hand the listen socket, every connection and a state snapshot to the
   fdstore so the next broker carries on where this one stopped. */
static void store_and_exit(int lfd, int sigfd) {
    g_draining = 1;
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, lfd, NULL);
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, sigfd, NULL);
    for (int i = 0; i < g_nconns; i++) if (g_conns[i]->authed) process_inbound(g_conns[i]);
    reload_abort();
    while (g_acts->n > 0) {
        sdbus_held_msg *held = NULL; int nh = 0;
        sdbus__acts_pop(g_acts, 0, &held, &nh);
        fail_held(held, nh, DBUS_ERROR_SPAWN_FAILED, "Message bus is restarting");
    }
    long deadline = sdbus__now_ms() + 2000;
    struct epoll_event evs[64];
    for (;;) {
        for (int j = 0; j < g_nconns; j++)
            if (g_conns[j]->oq_over || (sdbus_conn_has_out(g_conns[j]) && flush_conn(g_conns[j]) < 0)) {
                remove_conn(g_conns[j]);
                j = -1;
            }
        int behind = 0;
        for (int j = 0; j < g_nconns; j++) { ep_update(g_conns[j]); behind += sdbus_conn_has_out(g_conns[j]); }
        if (!behind) break;
        long left = deadline - sdbus__now_ms();
        if (left > 0) {
            int nev = epoll_wait(g_epfd, evs, 64, (int)left);
            for (int i = 0; i < nev; i++)
                if (evs[i].events & (EPOLLHUP | EPOLLERR)) remove_conn(evs[i].data.ptr);
            continue;
        }
        int *ids = malloc(behind * sizeof *ids), nb = 0;
        for (int j = 0; j < g_nconns; j++) if (sdbus_conn_has_out(g_conns[j])) ids[nb++] = g_conns[j]->id;
        for (int j = 0; j < nb; j++) { sdbus_conn *c = conn_by_id(ids[j]); if (c) remove_conn(c); }
        free(ids);
    }
    if (g_notify_sock) {
        int npfd = 0, k = 0;
        for (int i = 0; i < g_nconns; i++) npfd += g_conns[i]->n_pending_fds;
        int *cfds = malloc((g_nconns + 1) * sizeof *cfds), *pfds = malloc((npfd + 1) * sizeof *pfds);
        for (int i = 0; i < g_nconns; i++) {
            cfds[i] = g_conns[i]->fd;
            for (int j = 0; j < g_conns[i]->n_pending_fds; j++) pfds[k++] = g_conns[i]->pending_fds[j];
        }
        sdbus_state_hdr h = { g_next_id, g_names->next_unique, g_bcast_serial };
        int mfd = memfd_create("schema-dbus-state", MFD_CLOEXEC);
        if (mfd < 0 || sdbus_state_write(mfd, &h, g_conns, g_nconns, g_names, g_replies) != 0
            || store_fds("listen", &lfd, 1) < 0 || store_fds("state", &mfd, 1) < 0
            || store_fds("conn", cfds, g_nconns) < 0 || store_fds("pfd", pfds, npfd) < 0)
            fprintf(stderr, "schema-dbus: fdstore hand-off failed: %s\n", strerror(errno));
        else
            fprintf(stderr, "schema-dbus: stored %d connections, %d passed fds for restart\n", g_nconns, npfd);
    }
    exit(0);
}

static int name_is(const char *p, size_t len, const char *s) { return len == strlen(s) && !memcmp(p, s, len); }

static int state_fd_ours(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && fcntl(fd, F_GET_SEALS) >= 0;
}

static int peer_matches(const sdbus_conn *c) {
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return c->uid == (uid_t)-1 && c->pid == 0;
    return cred.uid == c->uid && cred.gid == c->gid && cred.pid == c->pid;
}

/* PID 1 handed back what the last broker stored, from fd 3 in LISTEN_FDNAMES
   order. A good snapshot resumes every connection untouched; otherwise they are
   closed and the bus starts fresh, on the stored listen socket if there is one.
   Returns that listen fd, or -1 to bind as usual. */
static int resume_from_fds(const char *fdnames, int n) {
    int lfd = -1, sfd = -1, nconn = 0, npfd = 0, rc = -1;
    int *cfds = malloc((n + 1) * sizeof *cfds), *pfds = malloc((n + 1) * sizeof *pfds);
    const char *p = fdnames;
    for (int i = 0; i < n; i++) {
        int fd = 3 + i;
        const char *e = p ? strchr(p, ':') : NULL;
        size_t len = p ? (e ? (size_t)(e - p) : strlen(p)) : 0;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (name_is(p, len, "listen") && lfd < 0) lfd = fd;
        else if (name_is(p, len, "state") && sfd < 0) sfd = fd;
        else if (name_is(p, len, "conn")) cfds[nconn++] = fd;
        else if (name_is(p, len, "pfd")) pfds[npfd++] = fd;
        else close(fd);
        p = e ? e + 1 : NULL;
    }
    if (lfd >= 0) fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
    if (sfd >= 0) {
        sdbus_state_hdr h;
        sdbus_conn **cs = calloc(nconn + 1, sizeof *cs);
        int nc = 0;
        rc = state_fd_ours(sfd) ? sdbus_state_read(sfd, &h, cs, nconn, &nc, cfds, nconn, pfds, npfd, g_names, g_replies) : -3;
        for (int i = 0; rc == 0 && i < nc; i++)
            if (!peer_matches(cs[i])) {
                sdbus__state_drop(cs, nc);
                sdbus_names_free(g_names); g_names = sdbus_names_new();
                sdbus_replies_free(g_replies); g_replies = sdbus_replies_new();
                rc = -3;
            }
        if (rc == 0) {
            g_conns = realloc(g_conns, (g_nconns + nc + 1) * sizeof *g_conns);
            for (int i = 0; i < nc; i++) {
                fcntl(cs[i]->fd, F_SETFL, fcntl(cs[i]->fd, F_GETFL) | O_NONBLOCK);
                g_conns[g_nconns++] = cs[i];
                if (cs[i]->is_monitor) g_nmonitors++;
                ep_add(cs[i]);
            }
            g_next_id = h.next_id;
            g_bcast_serial = h.driver_serial;
            fprintf(stderr, "schema-dbus: resumed %d connections from the fdstore\n", nc);
        }
        free(cs);
        close(sfd);
    }
    if (rc != 0) {
        fprintf(stderr, "schema-dbus: fdstore state %s, starting fresh\n",
                sfd < 0 ? "missing" : rc == -2 ? "has an unknown version" : rc == -3 ? "fails the identity check" : "is malformed");
        for (int i = 0; i < nconn; i++) close(cfds[i]);
        for (int i = 0; i < npfd; i++) close(pfds[i]);
    }
    free(cfds); free(pfds);
    store_remove_all();
    return lfd;
}

static int make_listen_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    /* ensure the socket's parent dir exists (e.g. /run/dbus) — a fresh boot or a
       container may not have created it yet, and bind() would ENOENT otherwise. */
    char dir[sizeof ((struct sockaddr_un *)0)->sun_path];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; mkdir(dir, 0755); }
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        int live = connect(probe, (struct sockaddr *)&sa, sizeof sa) == 0;
        close(probe);
        if (live) { close(fd); errno = EADDRINUSE; return -1; }
    }
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    chmod(path, 0777);            /* system bus is world-connectable; policy gates use */
    if (listen(fd, 128) < 0) { close(fd); return -1; }
    return fd;
}

/* fork+exec an activatable service, dropping to its User= before exec. Returns
   the child pid, or -1 if fork failed. Matches stock dbus-daemon: clean env with
   DBUS_STARTER_*; all broker fds are CLOEXEC so exec closes them. */
static pid_t spawn_service(const sdbus_svc_ent *e, const char *bus_addr) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) return pid;

    /* --- child --- */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);   /* broker blocks SIGCHLD for its signalfd; don't leak it */
    setsid();
    {   /* nor the broker's OOM protection: an activated helper is ordinary */
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
        if (ofd >= 0) { (void)!write(ofd, "0\n", 2); close(ofd); }
    }
    struct passwd *pw = getpwnam(e->user);
    if (!pw) _exit(127);                 /* unknown User= -> fail closed, never run as root */
    if (sdbus_activate_should_drop_privs(g_system_bus, pw->pw_uid)) {
        if (initgroups(e->user, pw->pw_gid) != 0) _exit(127);
        if (setgid(pw->pw_gid) != 0) _exit(127);
        if (setuid(pw->pw_uid) != 0) _exit(127);
    }
    extern char **environ;
    char **env = sdbus_activate_build_env(g_system_bus, bus_addr, g_actenv ? g_actenv : environ);
    execve(e->argv[0], e->argv, env);
    _exit(127);                         /* exec failed */
}

int main(int argc, char **argv) {
    int system_bus = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--system")) system_bus = 1;
        else if (strcmp(argv[i], "--watch-names")) {
            fprintf(stderr, "usage: schema-dbus [--system] [--watch-names]\n");
            return 2;
        }
    }
    g_system_bus = system_bus;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--watch-names")) { g_system_bus = 1; return watch_names(); }

    const char *lpid = getenv("LISTEN_PID"), *lfds = getenv("LISTEN_FDS"), *lnames = getenv("LISTEN_FDNAMES");
    int nlisten = lpid && lfds && atoi(lpid) == (int)getpid() ? atoi(lfds) : 0;
    char *listen_names = nlisten > 0 ? strdup(lnames ? lnames : "") : NULL;
    if (getenv("NOTIFY_SOCKET")) g_notify_sock = strdup(getenv("NOTIFY_SOCKET"));
    unsetenv("LISTEN_PID"); unsetenv("LISTEN_FDS"); unsetenv("LISTEN_FDNAMES"); unsetenv("NOTIFY_SOCKET");

    const char *sock = getenv("SCHEMA_DBUS_SOCKET");
    if (!sock) sock = DEFAULT_SOCKET;
    snprintf(g_bus_addr, sizeof g_bus_addr, "unix:path=%s", sock);

    /* load the dissolved policy: prefer a precompiled file, else dissolve live */
    const char *polfile = getenv("SCHEMA_DBUS_POLICY");
    char *poltext = polfile ? slurp(polfile) : NULL;
    g_policy = sdbus_policy_parse(poltext ? poltext : SDBUS_NO_POLICY_FILE_DEFAULT);
    free(poltext);
    g_names = sdbus_names_new();
    g_replies = sdbus_replies_new();
    const char *svcdir = getenv("SCHEMA_DBUS_SVCDIR");
    const char *svcdirs_env = getenv("SCHEMA_DBUS_SVCDIRS");
    const char *maskfile = getenv("SCHEMA_DBUS_MASKFILE");

    struct passwd *self_pw = g_system_bus ? NULL : getpwuid(getuid());
    const char *default_user = g_system_bus ? "root" : (self_pw ? self_pw->pw_name : "root");

    if (svcdir && !svcdirs_env) {
        g_svcdirs[0] = svcdir;
        g_nsvcdirs = 1;
    } else {
        char *svcdirs_buf = strdup(svcdirs_env ? svcdirs_env : SDBUS_SVC_DIRS);   /* kept for ReloadConfig */
        for (char *p = strtok(svcdirs_buf, ":"); p && g_nsvcdirs < SDBUS_MAX_SVCDIRS; p = strtok(NULL, ":"))
            g_svcdirs[g_nsvcdirs++] = p;
    }
    g_maskfile = maskfile ? maskfile : SDBUS_MASK_FILE;
    g_default_user = default_user ? strdup(default_user) : "root";
    g_svctab = sdbus_svctab_parse_dirs_masked(g_svcdirs, g_nsvcdirs, g_maskfile, g_default_user);
    /* handed over by schema-dbus-run.sh; without a dissolver a reload rebuilds
       the activation table only */
    g_polfile = polfile;
    g_dissect = getenv("SCHEMA_DBUS_DISSECT");
    g_cache = getenv("SCHEMA_DBUS_CACHE");
    g_system_conf = getenv("SCHEMA_DBUS_SYSTEM_CONF") ? getenv("SCHEMA_DBUS_SYSTEM_CONF") : "/usr/share/dbus-1/system.conf";
    g_override_dir = getenv("SCHEMA_DBUS_OVERRIDE_DIR") ? getenv("SCHEMA_DBUS_OVERRIDE_DIR")
                                                       : "/usr/local/share/dbus-1/system-services";
    if (getenv("SCHEMA_DBUS_RELOAD_TIMEOUT_MS")) g_reload_timeout_ms = atol(getenv("SCHEMA_DBUS_RELOAD_TIMEOUT_MS"));
    if (getenv("SCHEMA_DBUS_DEBOUNCE_MS")) g_debounce_ms = atol(getenv("SCHEMA_DBUS_DEBOUNCE_MS"));
    watch_setup();
    if (!g_system_bus) {
        extern char **environ;
        g_actenv = sdbus_activate_env_dup(environ);
    }
    g_acts = sdbus_acts_new();
    fprintf(stderr, "schema-dbus: %d activatable services\n", g_svctab->n);

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    int lfd = nlisten > 0 ? resume_from_fds(listen_names, nlisten) : -1;
    free(listen_names);
    if (lfd < 0) lfd = make_listen_socket(sock);
    if (lfd < 0) { fprintf(stderr, "schema-dbus: cannot bind %s: %s\n", sock, strerror(errno)); return 1; }

    /* SIGCHLD via signalfd so activated children are reaped in the event loop */
    static int sigfd_marker;
    sigset_t scmask;
    sigemptyset(&scmask);
    sigaddset(&scmask, SIGCHLD);
    sigaddset(&scmask, SIGTERM);
    sigaddset(&scmask, SIGHUP);        /* reload; its default action would kill the bus */
    sigprocmask(SIG_BLOCK, &scmask, NULL);
    int sigfd = signalfd(-1, &scmask, SFD_NONBLOCK | SFD_CLOEXEC);

    struct epoll_event lev = {0};
    lev.events = EPOLLIN; lev.data.ptr = NULL;   /* NULL ptr marks the listen fd */
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &lev);
    struct epoll_event sev = {0};
    sev.events = EPOLLIN; sev.data.ptr = &sigfd_marker;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, sigfd, &sev);
    static int inotify_marker;
    if (g_ifd >= 0) {
        struct epoll_event iev = {0};
        iev.events = EPOLLIN; iev.data.ptr = &inotify_marker;
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_ifd, &iev);
    }

    int vmaj = 0, vmin = 0, vmic = 0;
    dbus_get_version(&vmaj, &vmin, &vmic);
    fprintf(stderr, "schema-dbus: listening on %s (system=%d, libdbus %d.%d.%d)\n",
            sock, system_bus, vmaj, vmin, vmic);

    struct epoll_event evs[64];
    for (;;) {
        /* block until an event, or wake at the next pending-reply deadline */
        int wait_ms = -1;
        long nd = sdbus_replies_next_deadline(g_replies);
        long ad = sdbus_acts_next_deadline(g_acts);
        if (ad >= 0 && (nd < 0 || ad < nd)) nd = ad;
        if (g_rl.pid && (nd < 0 || g_rl.deadline < nd)) nd = g_rl.deadline;
        if (g_debounce_at && (nd < 0 || g_debounce_at < nd)) nd = g_debounce_at;
        if (nd >= 0) { long now = sdbus__now_ms(); wait_ms = nd <= now ? 0 : (int)(nd - now); }
        int nev = epoll_wait(g_epfd, evs, 64, wait_ms);
        if (nev < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < nev; i++) {
            if (evs[i].data.ptr == NULL) {           /* listen socket */
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    add_conn(cfd);
                }
                continue;
            }
            if (evs[i].data.ptr == &inotify_marker) { watch_read(); continue; }
            if (evs[i].data.ptr == &sigfd_marker) {   /* SIGCHLD: reap activated children */
                struct signalfd_siginfo si;
                int term = 0, hup = 0;
                while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) {
                    term |= si.ssi_signo == SIGTERM;
                    hup |= si.ssi_signo == SIGHUP;
                }
                int st; pid_t p;
                while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
                    if (reload_reaped(p, st)) continue;
                    sdbus_pending_act *pa = sdbus_acts_by_pid(g_acts, (int)p);
                    if (!pa) continue;               /* exited after claiming its name: normal */
                    sdbus_held_msg *held = NULL; int nh = 0;
                    sdbus_acts_take(g_acts, pa->name, &held, &nh);
                    fail_held(held, nh, DBUS_ERROR_SPAWN_CHILD_EXITED,
                              "Activated service exited before acquiring its name");
                }
                if (term) store_and_exit(lfd, sigfd);
                if (hup) reload_request("sighup", 0, 0);
                continue;
            }
            sdbus_conn *c = evs[i].data.ptr;
            if (evs[i].events & (EPOLLHUP | EPOLLERR)) { remove_conn(c); continue; }
            if (evs[i].events & EPOLLIN)  { if (!on_readable(c)) continue; }  /* c may be freed */
            if (evs[i].events & EPOLLOUT) {
                if (flush_conn(c) < 0) { remove_conn(c); continue; }
                ep_update(c);
            }
        }
        /* time out abandoned pending replies: a caller whose callee never answered
           gets a NoReply instead of hanging, and the entry is freed. */
        if (g_replies->n > 0) {
            long now = sdbus__now_ms();
            int cap = g_replies->n;
            int *callers = malloc(cap * sizeof *callers);
            uint32_t *serials = malloc(cap * sizeof *serials);
            int ne = sdbus_replies_reap_expired(g_replies, now, callers, serials, cap);
            for (int i = 0; i < ne; i++)
                send_no_reply_to(callers[i], serials[i],
                    "Did not receive a reply within the bus timeout");
            free(callers); free(serials);
        }
        reload_check_timeout();
        if (g_debounce_at && sdbus__now_ms() >= g_debounce_at) {
            g_debounce_at = 0;
            reload_request("inotify", 0, 0);
        }
        /* time out activations whose service never claimed its name */
        for (;;) {
            long now = sdbus__now_ms();
            sdbus_held_msg *held = NULL; int nh = 0;
            if (!sdbus_acts_reap_expired(g_acts, now, &held, &nh)) break;
            fail_held(held, nh, DBUS_ERROR_TIMED_OUT,
                      "Activated service failed to acquire its name in time");
        }
        /* reap connections whose outbound backlog overflowed the cap. Swept after
           the event batch (and after the NoReply enqueues above) so no evs[] entry
           can reference a freed conn; looped because a reap's disconnect signals can
           push another slow reader over. */
        for (int swept = 1; swept; ) {
            swept = 0;
            for (int j = 0; j < g_nconns; j++)
                if (g_conns[j]->oq_over) { remove_conn(g_conns[j]); swept = 1; break; }
        }
    }
    return 0;
}
