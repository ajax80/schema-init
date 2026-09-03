#ifndef SDBUS_ROUTE_H
#define SDBUS_ROUTE_H

/* Routing decision over a parsed wire header (sdbus_wire_msg), so fd-bearing
   messages route without a full demarshal. Pure decision logic (no I/O),
   unit-testable with fake conns. Rules:
   - a requested reply (method_return/error with reply_serial) bypasses policy
     and routes to the caller recorded in the reply table (dropped if unmatched);
   - otherwise the send is policy-gated; a denial sets *denied;
   - a signal with a destination is unicast to that owner; a signal without one
     is broadcast to every conn whose match set accepts it;
   - a method_call/return/error with a destination is unicast to the owner, or
     sets *synth_error (ServiceUnknown) if the name has no owner;
   - a method_call expecting a reply records a pending-reply entry. */

#include "sdbus_wire.h"
#include "sdbus_policy.h"
#include "sdbus_names.h"
#include "sdbus_conn.h"
#include "sdbus_reply.h"
#include "sdbus_match.h"

static inline const char *sdbus__type_str(int t) {
    switch (t) {
        case SDBUS_TYPE_METHOD_CALL:   return "method_call";
        case SDBUS_TYPE_METHOD_RETURN: return "method_return";
        case SDBUS_TYPE_ERROR:         return "error";
        case SDBUS_TYPE_SIGNAL:        return "signal";
        default:                       return NULL;
    }
}

static inline int sdbus__route_resolve(sdbus_names *names, sdbus_conn **all,
                                       int n_all, const char *dest) {
    if (!dest) return -1;
    for (int i = 0; i < n_all; i++)
        if (all[i]->unique && !strcmp(all[i]->unique, dest)) return all[i]->id;
    return sdbus_names_owner(names, dest);
}

static inline int sdbus_route_targets(sdbus_wire_msg *msg, sdbus_conn *sender,
        sdbus_names *names, sdbus_conn **all, int n_all, sdbus_policy *pol,
        sdbus_replies *replies, int *synth_error, int *denied,
        int *targets, int max_targets) {
    *synth_error = 0;
    *denied = 0;
    const char *tstr = sdbus__type_str(msg->type);
    int is_reply = (msg->type == SDBUS_TYPE_METHOD_RETURN ||
                    msg->type == SDBUS_TYPE_ERROR) && msg->has_reply_serial;

    /* requested reply: bypass policy, route to the recorded caller. Disambiguate
       by the reply's destination — per-connection serials collide across callers,
       so (callee, reply_serial) alone can match the wrong pending call. */
    if (is_reply) {
        int want = sdbus__route_resolve(names, all, n_all, msg->destination);
        int caller = sdbus_replies_match(replies, sender->id, msg->reply_serial, want);
        if (caller < 0) return 0;                 /* no pending call -> drop */
        if (max_targets < 1) return 0;
        targets[0] = caller;
        return 1;
    }

    /* build the request view and resolve the destination's owned names */
    const char **dnames = NULL; int ndn = 0;
    int dest_owner = sdbus__route_resolve(names, all, n_all, msg->destination);
    if (dest_owner >= 0) ndn = sdbus_names_owned_by(names, dest_owner, &dnames);

    sdbus_req req;
    memset(&req, 0, sizeof req);
    req.op = "send";
    req.uid = (int)sender->uid;
    req.gids = sender->gids;
    req.n_gids = sender->n_gids;
    req.interface = msg->interface;
    req.member = msg->member;
    req.msgtype = tstr;
    req.path = msg->path;
    req.destination = msg->destination;
    req.dest_names = dnames;
    req.n_dest_names = ndn;
    req.has_reply_serial = msg->has_reply_serial;

    const char *verdict = sdbus_policy_eval(pol, &req);
    free(dnames);
    if (strcmp(verdict, "allow") != 0) { *denied = 1; return 0; }

    /* signals */
    if (msg->type == SDBUS_TYPE_SIGNAL) {
        if (msg->destination) {                    /* directed signal */
            if (dest_owner < 0) return 0;
            if (max_targets < 1) return 0;
            targets[0] = dest_owner;
            return 1;
        }
        int n = 0;                                 /* broadcast by match rules */
        for (int i = 0; i < n_all && n < max_targets; i++) {
            sdbus_conn *cc = all[i];
            if (cc->id == sender->id) continue;
            if (cc->matches && sdbus_match_signal(cc->matches, msg->interface,
                                                  msg->member, msg->path, msg->sender))
                targets[n++] = cc->id;
        }
        return n;
    }

    /* method_call / non-reply return/error with a destination -> unicast */
    if (dest_owner < 0) { *synth_error = 1; return 0; }
    if (max_targets < 1) return 0;
    targets[0] = dest_owner;

    /* record a pending reply for calls that expect one */
    if (msg->type == SDBUS_TYPE_METHOD_CALL && !(msg->flags & SDBUS_FLAG_NO_REPLY))
        sdbus_replies_record(replies, dest_owner, msg->serial, sender->id);

    return 1;
}

#endif
