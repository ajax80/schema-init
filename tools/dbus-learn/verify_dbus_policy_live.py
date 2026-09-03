import json, sys
from felt_policy import parse_policy, evaluate
from dissect_policy import dissolve_tree
from deny_log import parse_rejection

def _reject_keys(deny_req):
    # identity of a rejected send: (uid, well-known destination, interface,
    # member, msgtype). A monitored grant matching any of these was actually
    # denied by the daemon, not delivered.
    return (deny_req.get("uid"), deny_req.get("destination"),
            deny_req.get("interface"), deny_req.get("member"), deny_req.get("msgtype"))

def _grant_keys(req):
    # a grant may address the destination by unique name; match against every
    # well-known name it resolves to (and the raw destination as a fallback)
    dests = list(req.get("destination_names") or [])
    if req.get("destination"):
        dests.append(req.get("destination"))
    for d in dests:
        yield (req.get("uid"), d, req.get("interface"), req.get("member"), req.get("msgtype"))

def check_corpus(policy, grant_records, deny_reqs, uid_to_contexts=None):
    # uid_to_contexts is accepted for call-site compatibility but unused: the
    # engine's own evaluate()/_applicable() already resolves user- and
    # group-context applicability against the full, unfiltered policy — that
    # is the single source of truth, so no pre-filter is applied here.
    fps, fns = [], []
    grants = 0
    skipped_no_uid = 0
    reclassified_denied = 0
    # A BecomeMonitor eavesdropper receives every message the daemon PROCESSES,
    # including sends the daemon then rejects on policy — so "monitor saw it"
    # does NOT prove delivery. Reconcile: a monitored send whose identity also
    # appears in the rejection log was denied, not granted; drop it from the
    # false-negative check (it belongs to the deny side).
    reject_index = {_reject_keys(d) for d in deny_reqs}
    for r in grant_records:
        # "op" present -> already a request tuple; otherwise a raw JSONL
        # record whose "type" must be a recognized dbus message type
        if r.get("op") != "send" and r.get("type") not in ("method_call", "signal", "error", "method_return"):
            continue
        req = _as_request(r)
        # Un-adjudicable: the passive learner never resolved this sender's
        # credentials (a short-lived connection that vanished before the async
        # GetConnectionCredentials). user/group-context policy needs the uid the
        # daemon read from SCM_CREDENTIALS at connect, so we cannot fairly hold
        # the engine to a verdict here — this is missing corpus data, not a
        # divergence. The real bus has authoritative creds and never hits this.
        if req.get("uid") is None:
            skipped_no_uid += 1
            continue
        if any(k in reject_index for k in _grant_keys(req)):
            reclassified_denied += 1
            continue
        grants += 1
        if evaluate(policy, req) == "deny":
            fns.append(req)
    denials = 0
    for req in deny_reqs:
        denials += 1
        if evaluate(policy, req) == "allow":
            fps.append(req)
    return {"false_positives": fps, "false_negatives": fns, "grants": grants,
            "denials": denials, "skipped_no_uid": skipped_no_uid,
            "reclassified_denied": reclassified_denied}

def _as_request(r):
    if "op" in r:  # already a request tuple
        return r
    return {  # JSONL record -> request tuple (send view)
        "op": "send", "uid": r.get("uid"), "gids": r.get("gids", []),
        "name": None, "destination": r.get("destination"),
        "destination_names": r.get("destination_names", []),
        "interface": r.get("interface"), "member": r.get("member"),
        "msgtype": r.get("type"), "path": r.get("path"),
        "reply_serial": r.get("reply_serial"),
    }

def main():
    cap, log = sys.argv[1], sys.argv[2]
    policy = parse_policy(dissolve_tree("/usr/share/dbus-1/system.conf"))
    grants = [json.loads(l) for l in open(cap) if l.strip()]
    denies = [d for d in (parse_rejection(l) for l in open(log)) if d]
    res = check_corpus(policy, grants, denies)
    print(f"grants={res['grants']} denials={res['denials']} "
          f"reclassified_denied={res['reclassified_denied']} "
          f"skipped_no_uid={res['skipped_no_uid']} "
          f"false_positives={len(res['false_positives'])} "
          f"false_negatives={len(res['false_negatives'])}")
    for fp in res["false_positives"][:20]:
        print("  FALSE-POSITIVE (engine allowed a rejected msg):", fp)
    for fn in res["false_negatives"][:20]:
        print("  FALSE-NEGATIVE (engine denied a delivered msg):", fn)
    sys.exit(0 if not res["false_positives"] and not res["false_negatives"] else 1)

if __name__ == "__main__":
    main()
