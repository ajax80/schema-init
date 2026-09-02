import json, os, sys, time

_TYPE_NAMES = {1: "method_call", 2: "method_return", 3: "error", 4: "signal"}

def _name(v):
    return str(v) if v is not None else None

def _names_for(dbus_name, unique_to_names):
    # Resolve a bus name to the well-known name(s) policy is written against:
    # a unique name (:1.x) maps to whatever well-known names it owns; a
    # well-known name resolves to itself. dbus policy send_destination rules
    # only ever reference well-known names, so this is what the gate matches on.
    if dbus_name is None:
        return []
    if dbus_name.startswith(":"):
        return list(unique_to_names.get(dbus_name, []))
    return [dbus_name]

def _public_name_arg(msg):
    # Read arg[0] of a name-registry method (RequestName/ReleaseName). Bus
    # names are PUBLIC — this is the one deliberate body read; message payloads
    # are still never touched (body_shape stays signature-only).
    try:
        args = msg.get_args_list()
        return str(args[0]) if args else None
    except Exception:
        return None

def record_from_message(msg, unique_to_names, unique_to_creds):
    t = _TYPE_NAMES.get(msg.get_type(), "unknown")
    sender = _name(msg.get_sender())
    dest = _name(msg.get_destination())
    creds = unique_to_creds.get(sender, {})
    rec = {
        "ts_mono": time.monotonic(),
        "ts_real": time.time(),
        "type": t,
        "serial": msg.get_serial(),
        "reply_serial": msg.get_reply_serial() or None,
        "sender": sender,
        "sender_names": list(unique_to_names.get(sender, [])),
        "destination": dest,
        "destination_names": _names_for(dest, unique_to_names),   # well-known names of the callee
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
        rec["owns"] = {"op": rec["member"], "name": _public_name_arg(msg)}
    return rec

def apply_name_owner_changed(msg, unique_to_names):
    # Keep the unique->well-known registry live across the whole run by
    # following org.freedesktop.DBus.NameOwnerChanged (name, old, new). Without
    # this the map is only a startup snapshot and goes stale for every
    # connection that appears later (PackageKit, transient tools, etc).
    args = None
    try:
        args = msg.get_args_list()
        name, old_owner, new_owner = str(args[0]), str(args[1]), str(args[2])
    except Exception:
        return
    if name.startswith(":"):
        return  # unique-name churn, not a well-known ownership change
    if old_owner:
        lst = unique_to_names.get(old_owner)
        if lst and name in lst:
            lst.remove(name)
            if not lst:
                unique_to_names.pop(old_owner, None)
    if new_owner:
        lst = unique_to_names.setdefault(new_owner, [])
        if name not in lst:
            lst.append(name)

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
    unique_to_names = {}
    unique_to_creds = {}

    # Two PRIVATE connections: a monitor connection may never send (the spec
    # forbids it), so all driver reads (ListNames/GetNameOwner snapshot AND
    # the per-message lazy GetConnectionCredentials) go through a separate,
    # ordinary client connection.
    client_bus = dbus.SystemBus(private=True)
    drv = client_bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
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
            # keep the ownership registry current BEFORE recording, so a message
            # sees the well-known names that were live when it was sent
            if (msg.get_member() == "NameOwnerChanged"
                    and _name(msg.get_interface()) == "org.freedesktop.DBus"):
                apply_name_owner_changed(msg, unique_to_names)
            sender = _name(msg.get_sender())
            if sender is not None and sender not in unique_to_creds:
                unique_to_creds[sender] = _fetch_creds(iface, sender)
            r = record_from_message(msg, unique_to_names, unique_to_creds)
            rec.write(r)
        except Exception:
            pass  # never let a bad record kill the drain loop
        return  # do not block, do not reply

    # become a monitor on a SEPARATE private connection: it receives
    # everything but must never send (BecomeMonitor + add_message_filter
    # only; no driver calls on monitor_bus).
    monitor_bus = dbus.SystemBus(private=True)
    monitor_bus.set_exit_on_disconnect(False)
    monitor_drv = monitor_bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
    monitor_iface = dbus.Interface(monitor_drv, "org.freedesktop.DBus.Monitoring")
    monitor_bus.add_message_filter(on_message)
    monitor_iface.BecomeMonitor(dbus.Array([], signature="s"), dbus.UInt32(0))
    GLib.MainLoop().run()

if __name__ == "__main__":
    main()
