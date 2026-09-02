import json, sys
from felt_policy import parse_policy, evaluate
from dissect_policy import dissolve_tree
from deny_log import parse_rejection

def check_corpus(policy, grant_records, deny_reqs, uid_to_contexts=None):
    # uid_to_contexts is accepted for call-site compatibility but unused: the
    # engine's own evaluate()/_applicable() already resolves user- and
    # group-context applicability against the full, unfiltered policy — that
    # is the single source of truth, so no pre-filter is applied here.
    fps, fns = [], []
    grants = 0
    for r in grant_records:
        # "op" present -> already a request tuple; otherwise a raw JSONL
        # record whose "type" must be a recognized dbus message type
        if r.get("op") != "send" and r.get("type") not in ("method_call", "signal", "error", "method_return"):
            continue
        req = _as_request(r)
        grants += 1
        if evaluate(policy, req) == "deny":
            fns.append(req)
    denials = 0
    for req in deny_reqs:
        denials += 1
        if evaluate(policy, req) == "allow":
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

def main():
    cap, log = sys.argv[1], sys.argv[2]
    policy = parse_policy(dissolve_tree("/usr/share/dbus-1/system.conf"))
    grants = [json.loads(l) for l in open(cap) if l.strip()]
    denies = [d for d in (parse_rejection(l) for l in open(log)) if d]
    res = check_corpus(policy, grants, denies)
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
