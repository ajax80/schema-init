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
