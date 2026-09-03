#!/usr/bin/env python3
"""Emit the dissolved policy + a (request, expected-verdict) golden for the C
policy conformance gate. The C engine (sdbus_policy.h) must reproduce every
verdict this Python engine (felt_policy.evaluate) produces, over the identical
dissolved policy.

Usage: emit_conformance_golden.py CORPUS.jsonl DISSOLVED_OUT.txt GOLDEN_OUT.tsv

The golden is TSV so the C test parses it without a JSON library. Columns:
  verdict op uid gids(csv) interface member msgtype path destination dest_names(csv) name has_reply_serial
Empty column == the field was None/absent.
"""
import json, sys
from felt_policy import parse_policy, evaluate
from dissect_policy import dissolve_tree
from verify_dbus_policy_live import _as_request

SYSTEM_CONF = "/usr/share/dbus-1/system.conf"


def _csv(xs):
    return ",".join(str(x) for x in (xs or []))


def _s(x):
    return "" if x is None else str(x)


def main():
    corpus, dissolved_out, golden_out = sys.argv[1], sys.argv[2], sys.argv[3]
    dissolved = dissolve_tree(SYSTEM_CONF)
    policy = parse_policy(dissolved)
    with open(dissolved_out, "w") as f:
        f.write(dissolved)

    n = 0
    with open(corpus) as src, open(golden_out, "w") as out:
        for line in src:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            # mirror the live gate's grant filter
            if r.get("op") != "send" and r.get("type") not in (
                    "method_call", "signal", "error", "method_return"):
                continue
            req = _as_request(r)
            if req.get("uid") is None:      # un-adjudicable, skipped in the gate
                continue
            verdict = evaluate(policy, req)
            has_reply = 1 if req.get("reply_serial") is not None else 0
            row = [verdict, _s(req.get("op")), _s(req.get("uid")),
                   _csv(req.get("gids")), _s(req.get("interface")),
                   _s(req.get("member")), _s(req.get("msgtype")),
                   _s(req.get("path")), _s(req.get("destination")),
                   _csv(req.get("destination_names")), _s(req.get("name")),
                   str(has_reply)]
            out.write("\t".join(row) + "\n")
            n += 1
    sys.stderr.write("emitted %d golden rows\n" % n)


if __name__ == "__main__":
    main()
