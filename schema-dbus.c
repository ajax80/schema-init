/* schema-dbus: a wire-compatible D-Bus system-bus broker. libdbus is linked as
   a wire codec only; the event loop, connection lifecycle, auth, name registry,
   routing, reply-tracking and policy are all ours. Service activation is a v1.1
   stub. See docs/superpowers/specs/2026-09-02-schema-dbus-sp1-broker-design.md */

#include <dbus/dbus.h>
#include <sys/epoll.h>
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
static char           g_bus_addr[256];
#define SDBUS_SVC_DIR "/usr/share/dbus-1/system-services"
#define SDBUS_SPAWN_TIMEOUT_MS 25000

/* ---- epoll registration ---- */
static void ep_update(sdbus_conn *c) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | (sdbus_conn_has_out(c) ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
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
        if (ch->off >= ch->len) { free(ch->b); c->oq_head++; }
        else return 0;                                 /* partial; wait for writable */
    }
    /* queue drained; compact */
    c->n_oq = c->oq_head = 0;
    c->oq_bytes = 0;
    free(c->oq); c->oq = NULL;
    return 0;
}

/* ---- signal broadcast for name transitions ---- */
static void enqueue_signal(sdbus_conn *dst, DBusMessage *sig) {
    dbus_message_set_serial(sig, ++g_bcast_serial);
    dbus_message_set_sender(sig, SDBUS_DRIVER_NAME);
    if (dst->unique) dbus_message_set_destination(sig, dst->unique);
    char *b = NULL; int len = 0;
    if (dbus_message_marshal(sig, &b, &len)) {
        sdbus_conn_enqueue(dst, (unsigned char *)b, len, NULL, 0);
        dbus_free(b);
        ep_update(dst);
    }
}

static const char *unique_or_empty(int conn_id) {
    if (conn_id < 0) return "";
    const char *u = sdbus_names_unique(g_names, conn_id);
    return u ? u : "";
}

static void broadcast_transitions(void *ctx, sdbus_transition *t, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) {
        const char *name = t[i].name;
        const char *oldo = unique_or_empty(t[i].old_owner);
        const char *newo = unique_or_empty(t[i].new_owner);

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
            }
        }
    }
}

/* move c->out (driver/auth scratch bytes) into the ordered queue as one chunk */
static void drain_scratch(sdbus_conn *c) {
    if (c->out_len > 0) {
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
    if (dbus_message_marshal(err, &b, &len)) { sdbus_conn_enqueue(c, (unsigned char *)b, len, NULL, 0); dbus_free(b); }
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

/* drop this message's fds from the head of pending_fds; close them unless they
   were transferred to an outbound chunk (which will close them after sending). */
static void consume_msg_fds(sdbus_conn *c, int nfds, int transferred) {
    if (nfds < 0) nfds = 0;
    if (nfds > c->n_pending_fds) nfds = c->n_pending_fds;   /* never index past what we hold */
    if (!transferred) for (int i = 0; i < nfds; i++) close(c->pending_fds[i]);
    for (int i = nfds; i < c->n_pending_fds; i++) c->pending_fds[i - nfds] = c->pending_fds[i];
    c->n_pending_fds -= nfds;
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

    int to_driver = w->destination && !strcmp(w->destination, SDBUS_DRIVER_NAME);
    if (!w->destination && w->interface && !strcmp(w->interface, "org.freedesktop.DBus.Peer"))
        to_driver = 1;   /* Peer methods (Ping/GetMachineId) may omit the destination */

    if (to_driver) {
        /* driver methods never carry fds -> libdbus can demarshal to read args */
        sdbus_msg dm;
        if (sdbus_codec_take(raw, rawlen, &dm) == rawlen) {
            int rc = sdbus_driver_dispatch(&dm, c, g_names, g_conns, g_nconns,
                                           broadcast_transitions, NULL);
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

    int targets[MAX_TARGETS], synth = 0, denied = 0;
    int nt = sdbus_route_targets(w, c, g_names, g_conns, g_nconns, g_policy,
                                 g_replies, &synth, &denied, targets, MAX_TARGETS);
    int transferred = 0;
    if (denied) {
        synth_error_wire(c, w->serial, DBUS_ERROR_ACCESS_DENIED, "rejected by policy");
    } else if (synth) {
        synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "name has no owner");
    } else if (nt > 0) {
        unsigned char *bytes = NULL; int len = 0;
        if (sdbus_wire_reforward(raw, w, c->unique, &bytes, &len) == 0) {
            for (int i = 0; i < nt; i++) {
                sdbus_conn *dst = conn_by_id(targets[i]);
                if (!dst) continue;
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
    g_conns = realloc(g_conns, (g_nconns + 1) * sizeof *g_conns);
    g_conns[g_nconns++] = c;
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void remove_conn(sdbus_conn *c) {
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    /* a conn can primary-own up to every name on the bus; size the transition
       buffer to that so RequestName-many-then-disconnect can't overflow it */
    int tcap = g_names->n_names; int nt = 0;
    sdbus_transition *t = tcap > 0 ? malloc(tcap * sizeof *t) : NULL;
    sdbus_names_disconnect(g_names, c->id, t, &nt, tcap);
    c->unique = NULL;   /* names just freed this string; don't let signals deref it */
    if (nt) broadcast_transitions(NULL, t, nt);
    free(t);
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

static int make_listen_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
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
    setsid();
    struct passwd *pw = getpwnam(e->user);
    if (pw && pw->pw_uid != 0) {
        if (initgroups(e->user, pw->pw_gid) != 0) _exit(127);
        if (setgid(pw->pw_gid) != 0) _exit(127);
        if (setuid(pw->pw_uid) != 0) _exit(127);
    }
    char starter[320];
    snprintf(starter, sizeof starter, "DBUS_STARTER_ADDRESS=%s", bus_addr);
    char *env[] = {
        (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin",
        starter,
        (char *)"DBUS_STARTER_BUS_TYPE=system",
        NULL
    };
    execve(e->argv[0], e->argv, env);
    _exit(127);                         /* exec failed */
}

int main(int argc, char **argv) {
    int system_bus = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--system")) system_bus = 1;

    const char *sock = getenv("SCHEMA_DBUS_SOCKET");
    if (!sock) sock = DEFAULT_SOCKET;
    snprintf(g_bus_addr, sizeof g_bus_addr, "unix:path=%s", sock);

    /* load the dissolved policy: prefer a precompiled file, else dissolve live */
    const char *polfile = getenv("SCHEMA_DBUS_POLICY");
    char *poltext = NULL;
    if (polfile) {
        FILE *f = fopen(polfile, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            poltext = malloc(n + 1);
            if (fread(poltext, 1, n, f) != (size_t)n) {
                free(poltext);
                poltext = NULL;
            } else {
                poltext[n] = '\0';
            }
            fclose(f);
        }
    }
    g_policy = sdbus_policy_parse(poltext ? poltext : "context = default\nallow = send_destination:*\n");
    free(poltext);
    g_names = sdbus_names_new();
    g_replies = sdbus_replies_new();
    g_svctab = sdbus_svctab_parse_dir(SDBUS_SVC_DIR);
    g_acts = sdbus_acts_new();
    fprintf(stderr, "schema-dbus: %d activatable services\n", g_svctab->n);

    int lfd = make_listen_socket(sock);
    if (lfd < 0) { fprintf(stderr, "schema-dbus: cannot bind %s: %s\n", sock, strerror(errno)); return 1; }

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event lev = {0};
    lev.events = EPOLLIN; lev.data.ptr = NULL;   /* NULL ptr marks the listen fd */
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &lev);

    int vmaj = 0, vmin = 0, vmic = 0;
    dbus_get_version(&vmaj, &vmin, &vmic);
    fprintf(stderr, "schema-dbus: listening on %s (system=%d, libdbus %d.%d.%d)\n",
            sock, system_bus, vmaj, vmin, vmic);

    struct epoll_event evs[64];
    for (;;) {
        /* block until an event, or wake at the next pending-reply deadline */
        int wait_ms = -1;
        long nd = sdbus_replies_next_deadline(g_replies);
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
