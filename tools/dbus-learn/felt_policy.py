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
        uid = request.get("uid")
        if uid is None:
            return False
        selector = context.selector
        if selector is None:
            return True
        # Check if uid matches selector: try numeric comparison, then name lookup
        try:
            return int(selector) == uid
        except ValueError:
            import pwd
            try:
                return pwd.getpwnam(selector).pw_uid == uid
            except KeyError:
                return False
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
