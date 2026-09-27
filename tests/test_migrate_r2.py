#!/usr/bin/env python3
"""arm-flip / advance-finish (R2) tests — script-style."""
import os, sys, tempfile, importlib.util
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _load():
    MOD = os.path.join(REPO, "distros/fedora-installer/migrate/schema-migrate.py")
    spec = importlib.util.spec_from_file_location("schema_migrate_r2", MOD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

def _root():
    r = tempfile.mkdtemp()
    for d in ("var/lib", "etc/xdg/autostart"):
        os.makedirs(os.path.join(r, d))
    open(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop"), "w").close()
    return r

def _ok(*a):
    class R: returncode = 0; stdout = ""; stderr = ""
    return R()
def _fail(*a):
    class R: returncode = 1; stdout = ""; stderr = ""
    return R()

# arm_flip requires R1_HEAL and advances to R2_PENDING
m = _load(); r = _root()
m.stage.write_stage(m.stage.R1_HEAL, root=r)
check("arm_flip returns 0 from R1_HEAL", m.arm_flip(root=r, flip=lambda *a: _ok()) == 0)
check("arm_flip advances to R2_PENDING", m.stage.read_stage(r) == m.stage.R2_PENDING)

# finish from R1_PENDING advances to R1_HEAL
m = _load(); r = _root()
m.stage.write_stage(m.stage.R1_PENDING, root=r)
check("finish R1_PENDING -> R1_HEAL", m.advance_finish(root=r, flip=lambda *a: _ok()) == m.stage.R1_HEAL)

# finish from R2_PENDING, authoritative, dbus opted out -> DONE + teardown
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r, extra={"adv": ["no-dbus-broker"]})
out = m.advance_finish(root=r, flip=lambda *a: _ok())     # is-authoritative rc 0
check("finish R2 authoritative -> DONE", out == m.stage.DONE)
check("finish R2 tears down autostart", not os.path.exists(os.path.join(r, "etc/xdg/autostart/schema-wizard.desktop")))

# finish from R2_PENDING, not authoritative -> ROLLED_BACK
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r)
out = m.advance_finish(root=r, flip=lambda *a: _fail())   # is-authoritative rc 1
check("finish R2 not-authoritative -> ROLLED_BACK", out == m.stage.ROLLED_BACK)

def _rec(answers=None):
    calls = []
    def flip(*a):
        calls.append(a[0])
        rc, out = (answers or {}).get(a[0], (0, ""))
        class R: pass
        R.returncode = rc; R.stdout = out; R.stderr = ""
        return R()
    return flip, calls

AUTOSTART = "etc/xdg/autostart/schema-wizard.desktop"

# R2 authoritative must CONFIRM the udev flip, or the seatbelt rolls it back on
# the next boot (desktop never confirmed across 2 armed boots)
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r)
flip, calls = _rec()
m.advance_finish(root=r, flip=flip)
check("finish R2 authoritative confirms the udev flip", "confirm" in calls)

# dbus offered by default: DONE keeps the wizard autostart for the R3 offer
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r, extra={"adv": []})
m.advance_finish(root=r, flip=_rec()[0])
check("DONE keeps autostart when dbus is offered", os.path.exists(os.path.join(r, AUTOSTART)))
check("dbus_offered true by default", m.dbus_offered(root=r))

# deploy-time opt-out survives the stage transitions
m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r, extra={"adv": ["no-dbus-broker"]})
m.advance_finish(root=r, flip=_rec()[0])
check("opt-out carried to DONE", not m.dbus_offered(root=r))
check("DONE tears down when dbus opted out", not os.path.exists(os.path.join(r, AUTOSTART)))
flip, calls = _rec()
check("arm_dbus refuses when opted out", m.arm_dbus(root=r, flip=flip) != 0 and "dbus-arm" not in calls)

# arm_dbus: DONE -> R3_PENDING via dbus-arm
m = _load(); r = _root()
m.stage.write_stage(m.stage.DONE, root=r)
flip, calls = _rec()
check("arm_dbus returns 0 from DONE", m.arm_dbus(root=r, flip=flip) == 0)
check("arm_dbus runs dbus-arm", calls == ["dbus-arm"])
check("arm_dbus advances to R3_PENDING", m.stage.read_stage(r) == m.stage.R3_PENDING)

m = _load(); r = _root()
m.stage.write_stage(m.stage.DONE, root=r)
flip, calls = _rec({"dbus-arm": (1, "")})
check("arm_dbus failure keeps DONE", m.arm_dbus(root=r, flip=flip) != 0 and m.stage.read_stage(r) == m.stage.DONE)

m = _load(); r = _root()
m.stage.write_stage(m.stage.R2_PENDING, root=r)
check("arm_dbus refuses before DONE", m.arm_dbus(root=r, flip=_rec()[0]) != 0)

# confirm is a no-op until the machine has actually rebooted into the flip
m = _load(); r = _root()
m.stage.write_stage(m.stage.DONE, root=r)
m._boot_id = lambda: "boot-a"
m.arm_dbus(root=r, flip=_rec()[0])
flip, calls = _rec({"dbus-is-authoritative": (1, ""), "dbus-state": (0, "armed\n")})
check("same-boot confirm does nothing", m.confirm_dbus(root=r, flip=flip) == m.stage.R3_PENDING and not calls)
check("same boot reports awaiting reboot", m.dbus_awaiting_reboot(root=r))
m._boot_id = lambda: "boot-b"
check("after reboot not awaiting", not m.dbus_awaiting_reboot(root=r))
flip, calls = _rec()
check("after reboot confirm proceeds", m.confirm_dbus(root=r, flip=flip) == m.stage.R3_DONE)

# finish at boot never decides R3 — the desktop login does
m = _load(); r = _root()
m.stage.write_stage(m.stage.R3_PENDING, root=r)
flip, calls = _rec()
check("finish leaves R3_PENDING alone", m.advance_finish(root=r, flip=flip) == m.stage.R3_PENDING and not calls)

# confirm_dbus: authoritative -> confirm, R3_DONE, teardown
m = _load(); r = _root()
m.stage.write_stage(m.stage.R3_PENDING, root=r)
flip, calls = _rec()
check("confirm_dbus authoritative -> R3_DONE", m.confirm_dbus(root=r, flip=flip) == m.stage.R3_DONE)
check("confirm_dbus confirms", "dbus-confirm" in calls and "dbus-rollback" not in calls)
check("R3_DONE tears down", not os.path.exists(os.path.join(r, AUTOSTART)))

# seatbelt already rolled back -> R3_ROLLED_BACK, no second rollback/reboot
m = _load(); r = _root()
m.stage.write_stage(m.stage.R3_PENDING, root=r)
flip, calls = _rec({"dbus-is-authoritative": (1, ""), "dbus-state": (0, "skipped\n")})
check("confirm_dbus after seatbelt -> R3_ROLLED_BACK", m.confirm_dbus(root=r, flip=flip) == m.stage.R3_ROLLED_BACK)
check("no second rollback or reboot", "dbus-rollback" not in calls and "reboot" not in calls)

# neither -> roll back ourselves and reboot
m = _load(); r = _root()
m.stage.write_stage(m.stage.R3_PENDING, root=r)
flip, calls = _rec({"dbus-is-authoritative": (1, ""), "dbus-state": (0, "armed\n")})
check("confirm_dbus unhealthy -> R3_ROLLED_BACK", m.confirm_dbus(root=r, flip=flip) == m.stage.R3_ROLLED_BACK)
check("rolls back then reboots", calls[-2:] == ["dbus-rollback", "reboot"] and "dbus-confirm" not in calls)

# skip_dbus: DONE stays DONE, offer withdrawn, teardown
m = _load(); r = _root()
m.stage.write_stage(m.stage.DONE, root=r)
check("skip_dbus returns 0", m.skip_dbus(root=r) == 0)
check("skip_dbus withdraws the offer", m.stage.read_stage(r) == m.stage.DONE and not m.dbus_offered(root=r))
check("skip_dbus tears down", not os.path.exists(os.path.join(r, AUTOSTART)))

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
