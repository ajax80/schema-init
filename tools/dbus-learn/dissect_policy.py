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
