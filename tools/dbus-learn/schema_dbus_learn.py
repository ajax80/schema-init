import json, os, sys, time

_TYPE_NAMES = {1: "method_call", 2: "method_return", 3: "error", 4: "signal"}

def _name(v):
    return str(v) if v is not None else None

def record_from_message(msg, unique_to_names, unique_to_creds):
    t = _TYPE_NAMES.get(msg.get_type(), "unknown")
    sender = _name(msg.get_sender())
    creds = unique_to_creds.get(sender, {})
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
        "uid": creds.get("uid"),
        "gids": list(creds.get("gids", [])),
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

def _fetch_creds(iface, sender):
    import dbus
    try:
        info = iface.GetConnectionCredentials(sender)
        uid = info.get("UnixUserID")
        gids = info.get("UnixGroupIDs")
        return {
            "uid": int(uid) if uid is not None else None,
            "gids": [int(g) for g in gids] if gids is not None else [],
        }
    except dbus.DBusException:
        return {"uid": None, "gids": []}

def main():
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    os.nice(19)
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/dbus-corpus/capture.jsonl"
    out_dir = os.path.dirname(out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    rec = Recorder(out)
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    unique_to_names = {}
    unique_to_creds = {}

    drv = bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
    iface = dbus.Interface(drv, "org.freedesktop.DBus")

    # seed initial ownership snapshot (+ best-effort creds)
    for wk in iface.ListNames():
        if not str(wk).startswith(":"):
            try:
                owner = str(iface.GetNameOwner(wk))
                unique_to_names.setdefault(owner, []).append(str(wk))
                unique_to_creds.setdefault(owner, _fetch_creds(iface, owner))
            except dbus.DBusException:
                pass

    def on_message(_bus, msg):
        try:
            sender = _name(msg.get_sender())
            if sender is not None and sender not in unique_to_creds:
                unique_to_creds[sender] = _fetch_creds(iface, sender)
            r = record_from_message(msg, unique_to_names, unique_to_creds)
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
