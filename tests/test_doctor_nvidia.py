#!/usr/bin/env python3
"""nvidia-wayland-egl tests — fake EGL dir + device node under DOCTOR_ROOT, no root."""
import os, sys, json, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
EGL = os.path.join(root, "usr/share/egl/egl_external_platform.d")
os.makedirs(EGL)
os.makedirs(os.path.join(root, "dev"))
WAYLAND = os.path.join(EGL, "10_nvidia_wayland.json")
GBM = os.path.join(EGL, "15_nvidia_gbm.json")
NODE = os.path.join(root, "dev/nvidia0")

spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.NvidiaWaylandEgl()

# no nvidia device → irrelevant → clean
check("clean when no nvidia node", c.detect() is None)

# nvidia present but egl-wayland not installed (no gbm sibling) → not our wound
open(NODE, "w").close()
check("clean when egl-wayland absent", c.detect() is None)

# egl-wayland installed (gbm sibling) and wayland json present → healthy
open(GBM, "w").close()
open(WAYLAND, "w").close()
check("clean when wayland json present", c.detect() is None)

# the wound: wayland registration deleted, gbm sibling still there
os.remove(WAYLAND)
f = c.detect()
check("detect flags missing registration", f is not None)
check("detail names the wayland json", f is not None and "10_nvidia_wayland.json" in f.detail)
check("finding is healable", f is not None and f.healable)

# heal restores it; verify passes; content is valid + points at the nvidia lib
snap = c.snapshot()
c.heal(f)
check("heal restores the json", os.path.exists(WAYLAND))
check("verify true after heal", c.verify() is True)
data = json.load(open(WAYLAND))
check("json points at libnvidia-egl-wayland", data["ICD"]["library_path"] == "libnvidia-egl-wayland.so.1")
check("clean once restored", c.detect() is None)

# back_out removes the file we created
c.back_out(snap)
check("back_out removes restored json", not os.path.exists(WAYLAND))

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
