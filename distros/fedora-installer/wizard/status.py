import json


def parse_status(text):
    try:
        d = json.loads(text)
        if not isinstance(d, dict):
            raise ValueError
    except (ValueError, TypeError):
        return {"overall": "UNKNOWN", "items": []}
    items = []
    for c in d.get("checks", []) or []:
        items.append({"name": c.get("name", ""), "color": c.get("color", ""),
                      "detail": c.get("detail", ""), "action": c.get("action", "")})
    return {"overall": d.get("overall", "UNKNOWN"), "items": items}


_REASONS = [
    ("schema-udev not running", "the new device manager didn't come up, so we put the old one back"),
    ("missing core node", "an essential system device was missing, so we put the old setup back"),
    ("no /dev/disk/by-uuid", "the disk wasn't showing up the way the system expects, so we reverted"),
    ("no /dev/input", "the keyboard and mouse weren't ready, so we reverted"),
    ("no group-accessible", "the graphics device wasn't reachable by the desktop, so we reverted"),
    ("desktop never confirmed", "the desktop didn't finish coming up in time, so we put things back"),
]


def humanize_reason(raw):
    r = (raw or "").strip()
    for needle, plain in _REASONS:
        if needle in r:
            return plain
    return "something didn't come up cleanly after the switch, so your computer put itself back"
