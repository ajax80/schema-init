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
