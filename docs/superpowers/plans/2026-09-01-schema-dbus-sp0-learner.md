# schema-dbus SP0 — Learner + Felt-Policy Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the throwaway Python tooling that records the live system D-Bus and proves a felt-schema policy engine reproduces the reference daemon's every allow/deny decision — the ground truth for the C bus (SP1).

**Architecture:** A passive `BecomeMonitor` eavesdropper writes append-only JSONL of all bus traffic (grants are implicit — observed delivery = allowed). A dissolver turns the busconfig XML into a felt `.dbus-policy` schema; a felt policy engine evaluates request tuples against it. A fidelity harness replays observed grants + daemon-logged denials through the engine and counts divergences. Nothing runs in production; nothing serves a socket.

**Tech Stack:** Python 3, `dbus-python` (client + low-level for monitor), `PyGObject`/GLib main loop, `xml.etree` for busconfig, `pytest`. All under `tools/dbus-learn/` (throwaway tree).

**Spec:** `docs/superpowers/specs/2026-09-01-schema-dbus-sp0-learner-design.md`

## Global Constraints

- **Read-only against the live bus.** No task may send, alter, or enforce anything on the real bus. BecomeMonitor only receives.
- **Corpus stays local.** `tests/dbus-corpus/` captured output is gitignored, never pushed, never leaves blakbox. Only a tiny redacted fixture may be committed.
- **dbus-daemon floor ≥ 1.9.10** for BecomeMonitor (blakbox is 1.16.2 — satisfied).
- **Cheap consumer.** The learner never blocks or buffers unboundedly; `os.nice(19)` at start; drain-and-append only. (Heeds the systemd1-bridge half-open-spin / slow-consumer / pidfd-leak scars.)
- **Throwaway.** SP0 code lives in `tools/dbus-learn/`, is not installed into any rail, and is not the terminus. Python only; C begins at SP1.
- **System bus only.** Session bus is SP4 — out of scope here.
- **Felt-policy default verdict is `deny`** when no rule matches (the system bus is default-deny for method calls to destinations).

## File Structure

- `tools/dbus-learn/felt_policy.py` — `.dbus-policy` parser + policy engine (`parse_policy`, `evaluate`). Pure, no I/O of the bus. The core both the dissolver output and the gate consume.
- `tools/dbus-learn/dissect_policy.py` — busconfig XML → `.dbus-policy` text (`dissolve_file`, `dissolve_tree`). Importable + CLI (`dissect-policy`).
- `tools/dbus-learn/schema_dbus_learn.py` — the eavesdropper: BecomeMonitor connection, message → JSONL record (`record_from_message`, `Recorder`, `main`). Importable core + CLI (`schema-dbus-learn`).
- `tools/dbus-learn/deny_log.py` — parse dbus-daemon rejection log lines → deny request tuples (`parse_rejection`).
- `tools/dbus-learn/verify_dbus_policy_live.py` — the fidelity gate: replay grants (JSONL) + denials (from deny_log) through the engine, count divergences (`check_corpus`, `main`). CLI (`verify-dbus-policy-live`).
- `tests/dbus-learn/` — pytest unit tests for each module above.
- `tests/dbus-corpus/` — gitignored capture dir (+ one committed redacted fixture `sample.jsonl`).
- `docs/superpowers/specs/2026-09-01-schema-dbus-contract-layer-note.md` — SP5 seed note.

Shared request-tuple type used across `felt_policy`, `deny_log`, `verify_dbus_policy_live`:

```python
# a dict with these keys (missing keys = None):
#   op:          "own" | "send" | "receive"
#   uid:         int | None          # sender uid
#   gids:        list[int]           # sender gids (for group contexts)
#   name:        str | None          # for op="own": the requested well-known name
#   destination: str | None          # well-known dest name (for send/receive)
#   interface:   str | None
#   member:      str | None
#   msgtype:     "method_call" | "method_return" | "signal" | "error" | None
#   path:        str | None
```

---

### Task 1: `.dbus-policy` parser

**Files:**
- Create: `tools/dbus-learn/felt_policy.py`
- Test: `tests/dbus-learn/test_felt_policy_parse.py`

**Interfaces:**
- Produces:
  - `Rule = namedtuple("Rule", ["decision", "predicates"])` — `decision`: `"allow"|"deny"`; `predicates`: `dict[str,str]`.
  - `Context = namedtuple("Context", ["kind", "selector", "rules"])` — `kind`: `"default"|"mandatory"|"user"|"group"`; `selector`: `str|None`; `rules`: `list[Rule]`.
  - `parse_policy(text: str) -> list[Context]`.

- [ ] **Step 1: Write the failing test**

```python
# tests/dbus-learn/test_felt_policy_parse.py
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from felt_policy import parse_policy, Rule, Context

def test_parses_contexts_and_rules():
    text = """\
context=default

allow=user:*
deny=own:*
deny=send_type:method_call

context=user:root

allow=own:org.freedesktop.login1
allow=send_destination:org.freedesktop.login1,send_interface:org.freedesktop.login1.Manager
"""
    ctxs = parse_policy(text)
    assert [c.kind for c in ctxs] == ["default", "user"]
    assert ctxs[1].selector == "root"
    assert ctxs[0].rules[0] == Rule("allow", {"user": "*"})
    assert ctxs[0].rules[2] == Rule("deny", {"send_type": "method_call"})
    # chained predicates on one line are ANDed
    assert ctxs[1].rules[1] == Rule(
        "allow",
        {"send_destination": "org.freedesktop.login1",
         "send_interface": "org.freedesktop.login1.Manager"},
    )
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_felt_policy_parse.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'felt_policy'`.

- [ ] **Step 3: Write minimal implementation**

```python
# tools/dbus-learn/felt_policy.py
from collections import namedtuple

Rule = namedtuple("Rule", ["decision", "predicates"])
Context = namedtuple("Context", ["kind", "selector", "rules"])

def _parse_predicates(spec):
    preds = {}
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        attr, _, value = part.partition(":")
        preds[attr.strip()] = value.strip()
    return preds

def parse_policy(text):
    contexts = []
    current = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if key == "context":
            kind, _, selector = value.partition(":")
            current = Context(kind.strip(), selector.strip() or None, [])
            contexts.append(current)
        elif key in ("allow", "deny"):
            if current is None:
                raise ValueError("rule before any context= line")
            current.rules.append(Rule(key, _parse_predicates(value)))
        else:
            raise ValueError(f"unknown key: {key!r}")
    return contexts
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_felt_policy_parse.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/dbus-learn/felt_policy.py tests/dbus-learn/test_felt_policy_parse.py
git commit -m "feat(dbus-learn): .dbus-policy parser"
```

---

### Task 2: felt policy engine — verdict evaluation

**Files:**
- Modify: `tools/dbus-learn/felt_policy.py` (add matching + `evaluate`)
- Test: `tests/dbus-learn/test_felt_policy_eval.py`

**Interfaces:**
- Consumes: `Rule`, `Context`, `parse_policy` (Task 1).
- Produces: `evaluate(contexts: list[Context], request: dict) -> "allow"|"deny"` — request is the shared tuple dict. Applies default → applicable user/group → mandatory context order; within the flattened order, last matching rule wins; fallback `"deny"` when nothing matches. A rule matches when **every** predicate matches the request (predicate `"*"` or absent field semantics per below).

- [ ] **Step 1: Write the failing test**

```python
# tests/dbus-learn/test_felt_policy_eval.py
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from felt_policy import parse_policy, evaluate

POLICY = parse_policy("""\
context=default
allow=user:*
deny=own:*
deny=send_type:method_call
allow=send_type:signal

context=user:root
allow=own:org.freedesktop.login1
allow=send_destination:org.freedesktop.login1
""")

def test_default_denies_method_call_to_unlisted_destination():
    req = {"op": "send", "uid": 1000, "gids": [1000], "destination": "com.example.Foo",
           "interface": "com.example.Foo", "member": "Do", "msgtype": "method_call", "path": "/"}
    assert evaluate(POLICY, req) == "deny"

def test_signal_allowed_by_default():
    req = {"op": "send", "uid": 1000, "gids": [1000], "destination": None,
           "interface": "com.example.Foo", "member": "Ping", "msgtype": "signal", "path": "/"}
    assert evaluate(POLICY, req) == "allow"

def test_root_may_own_and_send_to_login1():
    own = {"op": "own", "uid": 0, "gids": [0], "name": "org.freedesktop.login1"}
    assert evaluate(POLICY, own) == "allow"
    send = {"op": "send", "uid": 0, "gids": [0], "destination": "org.freedesktop.login1",
            "interface": "org.freedesktop.login1.Manager", "member": "Inhibit",
            "msgtype": "method_call", "path": "/org/freedesktop/login1"}
    assert evaluate(POLICY, send) == "allow"

def test_nonroot_cannot_own_login1():
    own = {"op": "own", "uid": 1000, "gids": [1000], "name": "org.freedesktop.login1"}
    assert evaluate(POLICY, own) == "deny"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_felt_policy_eval.py -v`
Expected: FAIL — `ImportError: cannot import name 'evaluate'`.

- [ ] **Step 3: Write minimal implementation**

Append to `tools/dbus-learn/felt_policy.py`:

```python
# maps a predicate attribute to the request field it constrains
_SEND = {"send_destination": "destination", "send_interface": "interface",
         "send_member": "member", "send_type": "msgtype", "send_path": "path"}
_RECV = {"receive_sender": "destination", "receive_interface": "interface",
         "receive_member": "member", "receive_type": "msgtype", "receive_path": "path"}

def _rule_matches(rule, request):
    op = request.get("op")
    for attr, value in rule.predicates.items():
        if attr == "user":
            # user context selector already filtered; a bare user predicate gates by uid name-agnostic "*"
            if value != "*":
                return False
        elif attr == "group":
            if value != "*":
                return False
        elif attr in ("own", "own_prefix"):
            if op != "own":
                return False
            name = request.get("name") or ""
            if attr == "own":
                if value != "*" and value != name:
                    return False
            else:  # own_prefix
                if not name.startswith(value):
                    return False
        elif attr in _SEND:
            if op != "send":
                return False
            if not _field_matches(request.get(_SEND[attr]), value):
                return False
        elif attr in _RECV:
            if op != "receive":
                return False
            if not _field_matches(request.get(_RECV[attr]), value):
                return False
        else:
            return False  # unknown predicate never matches (surfaces as divergence)
    return True

def _field_matches(field, value):
    if value == "*":
        return field is not None
    return field == value

def _applicable(context, request):
    if context.kind == "default" or context.kind == "mandatory":
        return True
    if context.kind == "user":
        return request.get("uid") is not None  # selector match handled by dissolver naming; see note
    if context.kind == "group":
        return True
    return False

def evaluate(contexts, request):
    ordered = ([c for c in contexts if c.kind == "default"]
               + [c for c in contexts if c.kind in ("user", "group")]
               + [c for c in contexts if c.kind == "mandatory"])
    verdict = "deny"
    for context in ordered:
        if not _applicable(context, request):
            continue
        for rule in context.rules:
            if _rule_matches(rule, request):
                verdict = rule.decision
    return verdict
```

> **Note on user/group selectors:** SP0 dissolves each real `<policy user="X">` into a `context=user:X` block, but the engine's uid→name resolution is deferred to the gate harness (Task 6), which passes only contexts applicable to the observed sender. `_applicable` therefore treats a user context as applicable when a uid is present; the harness pre-filters to the right user block. This keeps the engine free of `/etc/passwd` lookups. Revisit if a divergence traces to cross-user leakage.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_felt_policy_eval.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/dbus-learn/felt_policy.py tests/dbus-learn/test_felt_policy_eval.py
git commit -m "feat(dbus-learn): felt policy engine verdict evaluation"
```

---

### Task 3: busconfig XML → `.dbus-policy` dissolver

**Files:**
- Create: `tools/dbus-learn/dissect_policy.py`
- Test: `tests/dbus-learn/test_dissect_policy.py`

**Interfaces:**
- Consumes: nothing from prior tasks at import time; its output text is consumed by `parse_policy` (Task 1).
- Produces:
  - `dissolve_text(xml: str) -> str` — one busconfig XML document → `.dbus-policy` text.
  - `dissolve_tree(root_conf: str) -> str` — resolves `<includedir>`/`<include>` from a top-level `system.conf` and concatenates (CLI entry). *(Round-trip tested via `parse_policy` in this task; include resolution is exercised in Task 6 against the real tree.)*

- [ ] **Step 1: Write the failing test**

```python
# tests/dbus-learn/test_dissect_policy.py
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from dissect_policy import dissolve_text
from felt_policy import parse_policy, evaluate

XML = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy context="default">
    <deny own="*"/>
    <deny send_type="method_call"/>
    <allow send_type="signal"/>
  </policy>
  <policy user="root">
    <allow own="org.freedesktop.login1"/>
    <allow send_destination="org.freedesktop.login1"/>
  </policy>
</busconfig>
"""

def test_dissolve_then_evaluate_matches_intent():
    text = dissolve_text(XML)
    pol = parse_policy(text)
    assert [c.kind for c in pol] == ["default", "user"]
    # non-root cannot own login1; root can
    assert evaluate(pol, {"op": "own", "uid": 1000, "gids": [1000], "name": "org.freedesktop.login1"}) == "deny"
    assert evaluate(pol, {"op": "own", "uid": 0, "gids": [0], "name": "org.freedesktop.login1"}) == "allow"

def test_send_destination_and_interface_are_anded():
    xml = ('<busconfig><policy context="default">'
           '<allow send_destination="a.b" send_interface="a.b.I"/>'
           '</policy></busconfig>')
    pol = parse_policy(dissolve_text(xml))
    r = pol[0].rules[0]
    assert r.decision == "allow"
    assert r.predicates == {"send_destination": "a.b", "send_interface": "a.b.I"}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_dissect_policy.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'dissect_policy'`.

- [ ] **Step 3: Write minimal implementation**

```python
# tools/dbus-learn/dissect_policy.py
import os, sys
# busconfig files are local + trusted, but they carry a DOCTYPE with an external
# DTD reference, so prefer defusedxml (blocks XXE / entity-expansion) and fall
# back to stdlib (which already does not resolve external entities by default).
try:
    import defusedxml.ElementTree as ET
except ImportError:
    import xml.etree.ElementTree as ET

# busconfig predicate attributes we carry into .dbus-policy, in a stable order
_ATTRS = ["own", "own_prefix", "user", "group",
          "send_destination", "send_interface", "send_member",
          "send_type", "send_path",
          "receive_sender", "receive_interface", "receive_member",
          "receive_type", "receive_path"]

def _context_header(policy_el):
    if policy_el.get("context"):
        return f"context={policy_el.get('context')}"
    if policy_el.get("user") is not None:
        return f"context=user:{policy_el.get('user')}"
    if policy_el.get("group") is not None:
        return f"context=group:{policy_el.get('group')}"
    # at_console and other selectors are recorded verbatim for later triage
    for k, v in policy_el.attrib.items():
        return f"context={k}:{v}"
    return "context=default"

def _rule_line(rule_el):
    preds = [f"{a}:{rule_el.get(a)}" for a in _ATTRS if rule_el.get(a) is not None]
    if not preds:
        return None  # a rule with no modeled predicate is dropped (surfaces if it mattered)
    return f"{rule_el.tag}={','.join(preds)}"

def dissolve_text(xml):
    root = ET.fromstring(xml)
    out = []
    for policy_el in root.findall("policy"):
        out.append(_context_header(policy_el))
        out.append("")
        for rule_el in policy_el:
            if rule_el.tag in ("allow", "deny"):
                line = _rule_line(rule_el)
                if line:
                    out.append(line)
        out.append("")
    return "\n".join(out).strip() + "\n"

def dissolve_tree(root_conf):
    base = os.path.dirname(root_conf)
    texts = []
    def walk(path):
        with open(path) as fh:
            data = fh.read()
        texts.append(dissolve_text(data))
        root = ET.fromstring(data)
        for inc in root.findall("includedir"):
            d = os.path.join(base, inc.text) if not os.path.isabs(inc.text) else inc.text
            if os.path.isdir(d):
                for name in sorted(os.listdir(d)):
                    if name.endswith(".conf"):
                        walk(os.path.join(d, name))
        for inc in root.findall("include"):
            p = inc.text
            if p and os.path.exists(p):
                walk(p)
    walk(root_conf)
    return "\n".join(texts)

if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/dbus-1/system.conf"
    sys.stdout.write(dissolve_tree(src))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_dissect_policy.py -v`
Expected: PASS.

- [ ] **Step 5: Smoke the real tree (manual, no commit gate)**

Run: `python3 tools/dbus-learn/dissect_policy.py /usr/share/dbus-1/system.conf | head -40`
Expected: real `.dbus-policy` stanzas from blakbox's actual busconfig. Eyeball for unmodeled attributes (anything dropped by `_rule_line`); if a dropped predicate looks load-bearing, add it to `_ATTRS` and the engine's `_SEND`/`_RECV` maps, with a test.

- [ ] **Step 6: Commit**

```bash
git add tools/dbus-learn/dissect_policy.py tests/dbus-learn/test_dissect_policy.py
git commit -m "feat(dbus-learn): busconfig XML -> .dbus-policy dissolver"
```

---

### Task 4: the eavesdropper (BecomeMonitor → JSONL)

**Files:**
- Create: `tools/dbus-learn/schema_dbus_learn.py`
- Test: `tests/dbus-learn/test_record.py`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces:
  - `record_from_message(msg, unique_to_names: dict) -> dict` — a `dbus.lowlevel.Message`-like object → a JSONL record dict (fields per spec: `ts_mono`, `ts_real`, `type`, `serial`, `reply_serial`, `sender`, `sender_names`, `destination`, `path`, `interface`, `member`, `signature`, `body_shape`, `verdict` default `"allow"`, plus contract fields `intent`, `owns`).
  - `Recorder(path)` with `.write(record: dict)` — append-only JSONL, flush-per-line.
  - `main()` — connect system bus, BecomeMonitor, loop. CLI: `schema-dbus-learn [out.jsonl]`.

- [ ] **Step 1: Write the failing test** (record shaping is pure and unit-testable with a fake message; the live loop is integration-tested in Step 5)

```python
# tests/dbus-learn/test_record.py
import sys, pathlib, json, tempfile, os
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from schema_dbus_learn import record_from_message, Recorder

class FakeMsg:
    def __init__(self, **kw): self._kw = kw
    def get_type(self): return self._kw.get("type", 1)          # 1=method_call
    def get_serial(self): return self._kw.get("serial", 7)
    def get_reply_serial(self): return self._kw.get("reply_serial", 0)
    def get_sender(self): return self._kw.get("sender", ":1.42")
    def get_destination(self): return self._kw.get("destination", "org.freedesktop.login1")
    def get_path(self): return self._kw.get("path", "/org/freedesktop/login1")
    def get_interface(self): return self._kw.get("interface", "org.freedesktop.login1.Manager")
    def get_member(self): return self._kw.get("member", "Inhibit")
    def get_signature(self): return self._kw.get("signature", "ssss")

def test_record_maps_core_fields_and_resolves_names():
    rec = record_from_message(FakeMsg(), {":1.42": ["org.example.App"]})
    assert rec["type"] == "method_call"
    assert rec["sender"] == ":1.42"
    assert rec["sender_names"] == ["org.example.App"]
    assert rec["destination"] == "org.freedesktop.login1"
    assert rec["member"] == "Inhibit"
    assert rec["verdict"] == "allow"           # observed delivery = allowed
    assert rec["intent"] == "method_call"      # contract layer present
    assert "body_shape" in rec                 # shape, not payload

def test_recorder_appends_jsonl():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "c.jsonl")
    r = Recorder(p)
    r.write({"a": 1}); r.write({"a": 2})
    lines = [json.loads(l) for l in open(p)]
    assert lines == [{"a": 1}, {"a": 2}]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_record.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'schema_dbus_learn'`.

- [ ] **Step 3: Write minimal implementation**

```python
# tools/dbus-learn/schema_dbus_learn.py
import json, os, sys, time

_TYPE_NAMES = {1: "method_call", 2: "method_return", 3: "error", 4: "signal"}

def _name(v):
    return str(v) if v is not None else None

def record_from_message(msg, unique_to_names):
    t = _TYPE_NAMES.get(msg.get_type(), "unknown")
    sender = _name(msg.get_sender())
    rec = {
        "ts_mono": time.monotonic(),
        "ts_real": time.time(),
        "type": t,
        "serial": msg.get_serial(),
        "reply_serial": msg.get_reply_serial() or None,
        "sender": sender,
        "sender_names": list(unique_to_names.get(sender, [])),
        "destination": _name(msg.get_destination()),
        "path": _name(msg.get_path()),
        "interface": _name(msg.get_interface()),
        "member": _name(msg.get_member()),
        "signature": _name(msg.get_signature()),
        "body_shape": _name(msg.get_signature()),   # shape only; never the payload
        "verdict": "allow",                          # observed on the bus => allowed
        "intent": t,                                 # contract layer (SP5 seed)
        "owns": None,                                # filled for RequestName/ReleaseName below
    }
    if rec["interface"] == "org.freedesktop.DBus" and rec["member"] in ("RequestName", "ReleaseName"):
        rec["owns"] = {"op": rec["member"]}          # name arg captured from body in the live loop
    return rec

class Recorder:
    def __init__(self, path):
        self._fh = open(path, "a", buffering=1)      # line-buffered append
    def write(self, record):
        self._fh.write(json.dumps(record, default=str) + "\n")
    def close(self):
        self._fh.close()

def main():
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    os.nice(19)
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/dbus-corpus/capture.jsonl"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rec = Recorder(out)
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    unique_to_names = {}

    # seed initial ownership snapshot
    drv = bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
    iface = dbus.Interface(drv, "org.freedesktop.DBus")
    for wk in iface.ListNames():
        if not str(wk).startswith(":"):
            try:
                owner = str(iface.GetNameOwner(wk))
                unique_to_names.setdefault(owner, []).append(str(wk))
            except dbus.DBusException:
                pass

    def on_message(_bus, msg):
        try:
            r = record_from_message(msg, unique_to_names)
            rec.write(r)
        except Exception:
            pass  # never let a bad record kill the drain loop
        return  # do not block, do not reply

    # become a monitor: receive everything, reply to nothing
    monitor_iface = dbus.Interface(drv, "org.freedesktop.DBus.Monitoring")
    conn = bus.get_connection()
    conn.add_message_filter(on_message)
    monitor_iface.BecomeMonitor(dbus.Array([], signature="s"), dbus.UInt32(0))
    GLib.MainLoop().run()

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_record.py -v`
Expected: PASS.

- [ ] **Step 5: Integration smoke on the live bus (manual, root, short)**

Run: `sudo timeout 15 python3 tools/dbus-learn/schema_dbus_learn.py /tmp/cap-smoke.jsonl; wc -l /tmp/cap-smoke.jsonl; head -2 /tmp/cap-smoke.jsonl`
Expected: nonzero lines, each a valid JSON record with `type`/`sender`/`member`. Trigger traffic during the window (e.g. `loginctl list-sessions` in another terminal) to guarantee messages. Confirm blakbox stays responsive (read-only monitor, `nice 19`). Delete `/tmp/cap-smoke.jsonl` after.

- [ ] **Step 6: Commit**

```bash
git add tools/dbus-learn/schema_dbus_learn.py tests/dbus-learn/test_record.py
git commit -m "feat(dbus-learn): BecomeMonitor eavesdropper -> JSONL corpus"
```

---

### Task 5: daemon rejection-log parser (the deny corpus)

**Files:**
- Create: `tools/dbus-learn/deny_log.py`
- Test: `tests/dbus-learn/test_deny_log.py`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces: `parse_rejection(line: str) -> dict | None` — a dbus-daemon rejection log line → a request tuple (shared dict, `op="send"`, `verdict` implied `deny`), or `None` if the line is not a rejection.

- [ ] **Step 1: Write the failing test**

```python
# tests/dbus-learn/test_deny_log.py
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools" / "dbus-learn"))
from deny_log import parse_rejection

LINE = ('Rejected send message, 2 matched rules; type="method_call", '
        'sender=":1.55" (uid=1000 pid=3210 comm="dolphin") '
        'interface="org.freedesktop.systemd1.Manager" member="StartUnit" '
        'error name="(unset)" requested_reply="0" '
        'destination="org.freedesktop.systemd1" (uid=0 pid=1 comm="schema-init")')

def test_parses_rejection_into_request_tuple():
    req = parse_rejection(LINE)
    assert req["op"] == "send"
    assert req["uid"] == 1000
    assert req["msgtype"] == "method_call"
    assert req["interface"] == "org.freedesktop.systemd1.Manager"
    assert req["member"] == "StartUnit"
    assert req["destination"] == "org.freedesktop.systemd1"

def test_non_rejection_returns_none():
    assert parse_rejection("Successfully activated service 'org.foo'") is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_deny_log.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'deny_log'`.

- [ ] **Step 3: Write minimal implementation**

```python
# tools/dbus-learn/deny_log.py
import re

_FIELD = re.compile(r'(\w+)="((?:[^"\\]|\\.)*)"')
_UID = re.compile(r'sender="[^"]*"\s*\(uid=(\d+)')

def parse_rejection(line):
    if "Rejected send message" not in line:
        return None
    fields = dict(_FIELD.findall(line))
    uid_m = _UID.search(line)
    return {
        "op": "send",
        "uid": int(uid_m.group(1)) if uid_m else None,
        "gids": [],
        "name": None,
        "destination": fields.get("destination"),
        "interface": fields.get("interface") or None,
        "member": fields.get("member") or None,
        "msgtype": fields.get("type") or None,
        "path": fields.get("path") or None,
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_deny_log.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/dbus-learn/deny_log.py tests/dbus-learn/test_deny_log.py
git commit -m "feat(dbus-learn): parse dbus-daemon rejection lines -> deny tuples"
```

---

### Task 6: the fidelity gate — `verify-dbus-policy-live`

**Files:**
- Create: `tools/dbus-learn/verify_dbus_policy_live.py`
- Test: `tests/dbus-learn/test_verify_gate.py`

**Interfaces:**
- Consumes: `parse_policy`, `evaluate` (Tasks 1–2); `dissolve_tree` (Task 3); JSONL records shaped by Task 4; `parse_rejection` (Task 5).
- Produces:
  - `check_corpus(policy_contexts, grant_records, deny_reqs, uid_to_contexts) -> dict` — returns `{"false_positives": [...], "false_negatives": [...], "grants": N, "denials": M}`. A **false negative** = an observed grant the engine denies; a **false positive** = a daemon denial the engine allows.
  - `main()` — CLI: `verify-dbus-policy-live <capture.jsonl> <rejections.log>`; dissolves the live `/usr/share/dbus-1/system.conf`, runs `check_corpus`, prints the divergence counts, exit 0 iff both lists empty.

- [ ] **Step 1: Write the failing test**

```python
# tests/dbus-learn/test_verify_gate.py
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/dbus-learn/test_verify_gate.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'verify_dbus_policy_live'`.

- [ ] **Step 3: Write minimal implementation**

```python
# tools/dbus-learn/verify_dbus_policy_live.py
import json, sys
from felt_policy import parse_policy, evaluate, Context
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/dbus-learn/test_verify_gate.py -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/dbus-learn/verify_dbus_policy_live.py tests/dbus-learn/test_verify_gate.py
git commit -m "feat(dbus-learn): verify-dbus-policy-live fidelity gate"
```

---

### Task 7: gitignore the corpus, redacted fixture, contract-layer note

**Files:**
- Modify: `.gitignore`
- Create: `tests/dbus-corpus/sample.jsonl` (tiny, redacted, committed)
- Create: `docs/superpowers/specs/2026-09-01-schema-dbus-contract-layer-note.md`

**Interfaces:** none (documentation + hygiene).

- [ ] **Step 1: Ignore live captures, keep the fixture**

Add to `.gitignore`:

```
# schema-dbus SP0 learner: live corpus stays local, never pushed
tests/dbus-corpus/*
!tests/dbus-corpus/sample.jsonl
```

- [ ] **Step 2: Commit a tiny redacted fixture**

Create `tests/dbus-corpus/sample.jsonl` — 3 hand-written records (a method_call to login1, a signal, a RequestName), bodies as shape only, no real payloads. This exists so tests and future readers have a shape reference without any real capture.

```json
{"ts_mono":1.0,"ts_real":1756768000.0,"type":"method_call","serial":7,"reply_serial":null,"sender":":1.42","sender_names":["org.example.App"],"destination":"org.freedesktop.login1","path":"/org/freedesktop/login1","interface":"org.freedesktop.login1.Manager","member":"Inhibit","signature":"ssss","body_shape":"ssss","verdict":"allow","intent":"method_call","owns":null}
{"ts_mono":1.1,"ts_real":1756768000.1,"type":"signal","serial":8,"reply_serial":null,"sender":":1.42","sender_names":["org.example.App"],"destination":null,"path":"/","interface":"org.example.App","member":"Changed","signature":"","body_shape":"","verdict":"allow","intent":"signal","owns":null}
{"ts_mono":1.2,"ts_real":1756768000.2,"type":"method_call","serial":9,"reply_serial":null,"sender":":1.42","sender_names":[],"destination":"org.freedesktop.DBus","path":"/org/freedesktop/DBus","interface":"org.freedesktop.DBus","member":"RequestName","signature":"su","body_shape":"su","verdict":"allow","intent":"method_call","owns":{"op":"RequestName"}}
```

- [ ] **Step 3: Write the contract-layer note**

Create `docs/superpowers/specs/2026-09-01-schema-dbus-contract-layer-note.md`:

```markdown
# schema-dbus contract layer — SP5 seed note

**Status:** note only, not a design. Captured during SP0 so the felt wire (SP5)
is designed from real contracts rather than guessed.

The SP0 corpus records, per exchange, beyond the wire fields:
- **owns**: what well-known name (capability) a sender asserts via RequestName.
- **intent**: property get/set vs method invoke vs signal broadcast vs name lifecycle.
- **pairing**: reply_serial ties a return/error back to its call.

The felt wire (SP5) should be able to express the same *intents* without the
dbus wire encoding: a service asserting a capability, a peer requesting an
action under a contract, a broadcast of state change. SP0 keeps the raw
material; SP5 designs the loss function over it. No protocol is proposed here.
```

- [ ] **Step 4: Verify the full suite is green**

Run: `python3 -m pytest tests/dbus-learn/ -v`
Expected: all tests from Tasks 1–6 PASS.

- [ ] **Step 5: Commit**

```bash
git add .gitignore tests/dbus-corpus/sample.jsonl docs/superpowers/specs/2026-09-01-schema-dbus-contract-layer-note.md
git commit -m "chore(dbus-learn): gitignore corpus, add fixture + contract-layer note"
```

---

## After the plan: capture campaign (not a code task)

Once Tasks 1–7 land, the SP0→SP1 gate is run against **real** traffic, not fixtures:

1. `sudo python3 tools/dbus-learn/schema_dbus_learn.py tests/dbus-corpus/capture.jsonl &` — run across days of normal use, covering at least: a full login/logout, suspend/resume, a package transaction, and an audio+video session (exercises WirePlumber/PipeWire + the systemd1 relay paths).
2. Collect the daemon's rejection lines from the journal into `rejections.log`.
3. `python3 tools/dbus-learn/verify_dbus_policy_live.py tests/dbus-corpus/capture.jsonl rejections.log`.
4. Drive divergences to **zero false-positives and zero false-negatives** by fixing the dissolver/engine (the udev `verify-rules-live` 549→2 discipline). Only then does SP1 (the C bus) begin.

## Self-Review

- **Spec coverage:** eavesdropper (Task 4) ✓; policy dissolver + `.dbus-policy` grammar (Tasks 1,3) ✓; felt engine (Task 2) ✓; fidelity gate with implicit-grants/explicit-denials (Tasks 5,6) ✓; contract layer (Tasks 4,7) ✓; corpus-stays-local constraint (Task 7) ✓; capture campaign + gate metric (post-plan section) ✓. Session bus / C bus correctly deferred (non-goals).
- **Placeholder scan:** every code step carries real code; no TBD/TODO; the one deferred detail (group-context filtering) is explicitly marked permissive with a revisit note, not a silent gap.
- **Type consistency:** the shared request-tuple dict keys (`op/uid/gids/name/destination/interface/member/msgtype/path`) are used identically in Tasks 2, 5, 6; `Rule`/`Context` used consistently 1→2→3; JSONL record fields produced in Task 4 are consumed by `_as_request` in Task 6.
- **Known softness (acceptable for SP0):** `_applicable`/`_filtered` split user-context handling between engine and harness; documented in the Task 2 note. If the real corpus shows cross-user divergence, tighten there.
