import functools, pwd, grp
from collections import namedtuple

Rule = namedtuple("Rule", ["decision", "predicates"])
Context = namedtuple("Context", ["kind", "selector", "rules"])

# name->id resolution, cached: users/groups are stable within a gate run, and a
# large corpus re-resolves the same handful of selectors (root, avahi, ...)
# once per record without this — the difference between ~1min and >4min on 83k.
@functools.lru_cache(maxsize=None)
def _uid_of(name):
    try:
        return pwd.getpwnam(name).pw_uid
    except KeyError:
        return None

@functools.lru_cache(maxsize=None)
def _gid_of(name):
    try:
        return grp.getgrnam(name).gr_gid
    except KeyError:
        return None

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

# maps a predicate attribute to the request field it constrains.
# send_destination is handled separately (set-membership against the
# destination's owned well-known names), not through this scalar map.
_SEND = {"send_interface": "interface",
         "send_member": "member", "send_type": "msgtype", "send_path": "path"}
# receive_sender constrains the sender identity of the incoming message, not its
# destination. sender_name is not yet populated by any request builder in SP0
# (no op="receive" request exists yet) — this mapping is correctness-for-the-future.
_RECV = {"receive_sender": "sender_name", "receive_interface": "interface",
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
        elif attr == "send_destination":
            if op != "send":
                return False
            # dbus evaluates send_destination against the well-known name(s) the
            # destination connection OWNS. On the wire the destination is often a
            # unique name (:1.x); destination_names is its resolved ownership set
            # (from the learner's live registry). Fall back to the raw
            # destination for old corpus records that predate that field.
            names = request.get("destination_names")
            if not names:
                d = request.get("destination")
                names = [d] if d else []
            if value == "*":
                if not names:
                    return False
            elif value not in names:
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
        uid = request.get("uid")
        if uid is None:
            return False
        selector = context.selector
        if selector is None:
            return True
        # Check if uid matches selector: try numeric comparison, then name lookup.
        # NOTE: This deliberately performs uid/name resolution here rather than deferring
        # to the gate harness (Task 6), enabling the engine to work standalone on unfiltered
        # policy (as the brief's own tests require). The harness can pre-filter in production.
        try:
            return int(selector) == uid
        except ValueError:
            return _uid_of(selector) == uid
    if context.kind == "group":
        gids = request.get("gids")
        if not gids:
            return False
        selector = context.selector
        if selector is None:
            return True
        # Check if any gid matches selector: try numeric comparison, then name lookup.
        # NOTE: Same rationale as user contexts above — resolves selectors here for engine
        # standalone testing, while harness can pre-filter in production.
        try:
            target_gid = int(selector)
        except ValueError:
            target_gid = _gid_of(selector)
            if target_gid is None:
                return False
        return target_gid in gids
    return False

def _is_requested_reply(request):
    # A method_return/error carrying a reply_serial is a reply to a pending
    # call. dbus's send_requested_reply defaults true, so ordinary send policy
    # (send_destination denies in particular) does not apply to such replies.
    # The dissolver drops send_requested_reply="false" qualifiers (a documented
    # deferral), so no surviving rule can re-block a reply — "allow" is correct
    # for every requested reply the daemon actually delivered.
    return (request.get("op") == "send"
            and request.get("reply_serial") is not None
            and request.get("msgtype") in ("method_return", "error"))

def evaluate(contexts, request):
    if _is_requested_reply(request):
        return "allow"
    ordered = ([c for c in contexts if c.kind == "default"]
               + [c for c in contexts if c.kind == "group"]
               + [c for c in contexts if c.kind == "user"]
               + [c for c in contexts if c.kind == "mandatory"])
    verdict = "deny"
    for context in ordered:
        if not _applicable(context, request):
            continue
        for rule in context.rules:
            if _rule_matches(rule, request):
                verdict = rule.decision
    return verdict
