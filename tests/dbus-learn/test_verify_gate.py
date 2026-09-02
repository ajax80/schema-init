import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from felt_policy import parse_policy
from verify_dbus_policy_live import check_corpus

POLICY = parse_policy("""\
context=default
deny=send_type:method_call
allow=send_type:signal
context=user:root
allow=send_destination:org.freedesktop.login1
""")

def uid_ctx(uid):
    # harness pre-filter: which contexts apply to this sender uid
    return "root" if uid == 0 else None

def test_clean_corpus_has_no_divergence():
    grants = [
        {"op": "send", "uid": 0, "gids": [0], "destination": "org.freedesktop.login1",
         "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"},  # root allowed
        {"op": "send", "uid": 1000, "gids": [1000], "destination": None,
         "interface": "x", "member": "y", "msgtype": "signal", "path": "/"},       # signal allowed
    ]
    denies = [
        {"op": "send", "uid": 1000, "gids": [1000], "destination": "com.x",
         "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"},   # correctly denied
    ]
    res = check_corpus(POLICY, grants, denies, uid_ctx)
    assert res["false_positives"] == []
    assert res["false_negatives"] == []
    assert res["grants"] == 2 and res["denials"] == 1

def test_false_positive_is_caught():
    # engine would ALLOW something the daemon rejected -> dangerous divergence
    denies = [{"op": "send", "uid": 0, "gids": [0], "destination": "org.freedesktop.login1",
               "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"}]
    res = check_corpus(POLICY, [], denies, uid_ctx)
    assert len(res["false_positives"]) == 1

def test_false_negative_is_caught():
    # engine would DENY something the daemon actually delivered -> gate must flag it
    grants = [{"op": "send", "uid": 1000, "gids": [1000], "destination": "com.x",
               "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"}]
    res = check_corpus(POLICY, grants, [], uid_ctx)
    assert len(res["false_negatives"]) == 1

def test_bare_user_context_is_not_dropped():
    # A bare "context=user" (no selector) applies to EVERY uid per felt_policy's
    # own _applicable(). The retired pre-filter kept a context only when
    # selector == uid_to_contexts(uid); for a NAMED uid that resolves to a
    # non-None value, None == "somebody" is False, so it would have dropped
    # this bare context and missed the resulting deny. Use a stub that
    # resolves uid 1000 to a real name (unlike the module-level uid_ctx,
    # which happens to return None for uid 1000 and would coincidentally
    # survive the old filter) to actually exercise that failure mode.
    def resolves_to_a_name(uid):
        return "somebody"

    policy = parse_policy("""\
context=default
allow=send_type:method_call
context=user
deny=send_destination:com.x
""")
    grants = [{"op": "send", "uid": 1000, "gids": [1000], "destination": "com.x",
               "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"}]
    res = check_corpus(policy, grants, [], resolves_to_a_name)
    assert len(res["false_negatives"]) == 1

def test_monitored_but_rejected_send_is_reconciled_out_of_grants():
    # A BecomeMonitor eavesdropper sees sends the daemon then REJECTS on policy,
    # so the same message appears in both the monitor stream (as a "grant") and
    # the rejection log. Without reconciliation the engine's correct deny on it
    # would be miscounted as a false-negative. The reject index must reclassify
    # it as a denial instead.
    msg = {"op": "send", "uid": 1000, "gids": [1000], "destination": "com.x",
           "interface": "i", "member": "m", "msgtype": "method_call", "path": "/"}
    res = check_corpus(POLICY, [msg], [dict(msg)], uid_ctx)
    assert res["reclassified_denied"] == 1
    assert res["grants"] == 0
    assert res["false_negatives"] == []   # not counted against the engine
    assert res["false_positives"] == []   # engine denies it on the deny side

def test_reconcile_matches_via_resolved_destination_name():
    # the grant addresses the callee by unique name; the rejection names the
    # well-known name. destination_names must bridge them so the reconcile hits.
    grant = {"op": "send", "uid": 1000, "gids": [1000], "destination": ":1.7",
             "destination_names": ["fi.w1.wpa_supplicant1"], "interface": "i",
             "member": "m", "msgtype": "method_call", "path": "/"}
    deny = {"op": "send", "uid": 1000, "gids": [1000], "destination": "fi.w1.wpa_supplicant1",
            "interface": "i", "member": "m", "msgtype": "method_call", "path": "/"}
    res = check_corpus(POLICY, [grant], [deny], uid_ctx)
    assert res["reclassified_denied"] == 1 and res["grants"] == 0

def test_uid_none_grant_is_skipped_not_counted():
    # a sender whose creds the passive learner never resolved is un-adjudicable:
    # skipped, never a false-negative.
    grant = {"op": "send", "uid": None, "gids": [], "destination": "com.x",
             "interface": "i", "member": "m", "msgtype": "method_call", "path": "/"}
    res = check_corpus(POLICY, [grant], [], uid_ctx)
    assert res["skipped_no_uid"] == 1
    assert res["grants"] == 0 and res["false_negatives"] == []

def test_requested_reply_is_not_a_false_negative():
    # a method_return with a reply_serial to a deny_destination service is a
    # requested reply -> the engine allows it, so it must NOT show as an FN even
    # though the default policy denies method_call sends to that destination.
    policy = parse_policy("""\
context=default
allow=send_type:method_return
deny=send_destination:com.x
""")
    reply = {"op": "send", "uid": 1000, "gids": [1000], "destination": "com.x",
             "destination_names": ["com.x"], "interface": "i", "member": "m",
             "msgtype": "method_return", "path": "/", "reply_serial": 7, "type": "method_return"}
    res = check_corpus(policy, [reply], [], uid_ctx)
    assert res["false_negatives"] == [] and res["grants"] == 1

def test_numeric_user_selector_matches_full_policy_evaluation():
    # context=user:0 must be resolved the same way the engine resolves it
    # directly (numeric compare against uid), for both a matching and a
    # non-matching uid.
    from felt_policy import evaluate
    policy = parse_policy("""\
context=default
deny=send_type:method_call
context=user:0
allow=send_destination:org.freedesktop.login1
""")
    root_req = {"op": "send", "uid": 0, "gids": [0], "destination": "org.freedesktop.login1",
                "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"}
    other_req = {"op": "send", "uid": 1000, "gids": [1000], "destination": "org.freedesktop.login1",
                 "interface": "x", "member": "y", "msgtype": "method_call", "path": "/"}

    # root_req is a real observed grant; other_req is a real observed denial
    res = check_corpus(policy, [root_req], [other_req], uid_ctx)
    assert res["false_negatives"] == []
    assert res["false_positives"] == []
    assert evaluate(policy, root_req) == "allow"
    assert evaluate(policy, other_req) == "deny"
