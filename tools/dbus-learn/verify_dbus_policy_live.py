import json, sys
from felt_policy import parse_policy, evaluate
from dissect_policy import dissolve_tree
from deny_log import parse_rejection

def _filtered(policy, uid, uid_to_contexts):
    """Keep default + mandatory always; keep the one user block that applies to this uid."""
    want_user = uid_to_contexts(uid)
    out = []
    for c in policy:
        if c.kind in ("default", "mandatory"):
            out.append(c)
        elif c.kind == "user" and c.selector == want_user:
            out.append(c)
        elif c.kind == "group":
            out.append(c)  # group filtering deferred; kept permissive for now
    return out

def check_corpus(policy, grant_records, deny_reqs, uid_to_contexts):
    fps, fns = [], []
    grants = 0
    for r in grant_records:
        if r.get("op") != "send" and r.get("type") not in ("method_call", "signal", "error", "method_return"):
            continue
        req = _as_request(r)
        grants += 1
        if evaluate(_filtered(policy, req.get("uid"), uid_to_contexts), req) == "deny":
            fns.append(req)
    denials = 0
    for req in deny_reqs:
        denials += 1
        if evaluate(_filtered(policy, req.get("uid"), uid_to_contexts), req) == "allow":
            fps.append(req)
    return {"false_positives": fps, "false_negatives": fns, "grants": grants, "denials": denials}

def _as_request(r):
    if "op" in r:  # already a request tuple
        return r
    return {  # JSONL record -> request tuple (send view)
        "op": "send", "uid": r.get("uid"), "gids": r.get("gids", []),
        "name": None, "destination": r.get("destination"),
        "interface": r.get("interface"), "member": r.get("member"),
        "msgtype": r.get("type"), "path": r.get("path"),
    }

def _uid_to_contexts(uid):
    import pwd
    if uid is None:
        return None
    try:
        return pwd.getpwuid(uid).pw_name
    except KeyError:
        return None

def main():
    cap, log = sys.argv[1], sys.argv[2]
    policy = parse_policy(dissolve_tree("/usr/share/dbus-1/system.conf"))
    grants = [json.loads(l) for l in open(cap) if l.strip()]
    denies = [d for d in (parse_rejection(l) for l in open(log)) if d]
    res = check_corpus(policy, grants, denies, _uid_to_contexts)
    print(f"grants={res['grants']} denials={res['denials']} "
          f"false_positives={len(res['false_positives'])} "
          f"false_negatives={len(res['false_negatives'])}")
    for fp in res["false_positives"][:20]:
        print("  FALSE-POSITIVE (engine allowed a rejected msg):", fp)
    for fn in res["false_negatives"][:20]:
        print("  FALSE-NEGATIVE (engine denied a delivered msg):", fn)
    sys.exit(0 if not res["false_positives"] and not res["false_negatives"] else 1)

if __name__ == "__main__":
    main()
