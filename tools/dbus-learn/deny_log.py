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
