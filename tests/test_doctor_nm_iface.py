#!/usr/bin/env python3
"""nm-profile-iface tests — fake /sys/class/net + NM keyfiles under DOCTOR_ROOT, no root."""
import os, sys, tempfile, importlib.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
root = tempfile.mkdtemp()
os.environ["DOCTOR_ROOT"] = root
NM = os.path.join(root, "etc/NetworkManager/system-connections")
os.makedirs(NM)


def nic(name, mac):
    d = os.path.join(root, "sys/class/net", name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "address"), "w") as fh:
        fh.write(mac + "\n")


def profile(fname, pid, typ, ifname="", mac="", sect="ethernet"):
    body = f"[connection]\nid={pid}\ntype={typ}\n"
    if ifname:
        body += f"interface-name={ifname}\n"
    if mac:
        body += f"\n[{sect}]\nmac-address={mac}\n"
    with open(os.path.join(NM, fname), "w") as fh:
        fh.write(body)


spec = importlib.util.spec_from_file_location(
    "schema_doctor", os.path.join(REPO, "scripts", "schema-doctor.py"))
sd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sd)

results = []
def check(n, ok): results.append(ok); print(f"  {'PASS' if ok else 'FAIL'}  {n}")

c = sd.NmProfileIface()

check("clean with no /sys/class/net", c.detect() is None)

nic("lo", "00:00:00:00:00:00")
nic("eth0", "a8:a1:59:0b:e8:ef")
nic("wlan0", "0a:b6:ba:6f:8c:80")

profile("wifi.nmconnection", "home", "wifi")
check("clean: wifi profile with no interface-name", c.detect() is None)

profile("eth0.nmconnection", "wired", "ethernet", ifname="eth0")
check("clean: profile names an existing iface", c.detect() is None)

profile("usb.nmconnection", "usb-dongle", "ethernet", ifname="enx001122", mac="00:11:22:33:44:55")
check("clean: stale name but its NIC is absent (MAC not present)", c.detect() is None)

profile("br.nmconnection", "docker-br", "bridge", ifname="br-gone")
check("clean: non-ethernet/wifi types ignored", c.detect() is None)

# the blakbox case: saved against the predictable name, MAC is on eth0
profile("enp6s0.nmconnection", "enp6s0", "ethernet", ifname="enp6s0", mac="A8:A1:59:0B:E8:EF")
f = c.detect()
check("flags profile whose name is gone but MAC is present", f is not None)
check("detail names the profile and wanted iface", f is not None and "'enp6s0' wants enp6s0" in f.detail)
check("detail points at the kernel name holding its MAC", f is not None and "on eth0" in f.detail)
check("finding not healable", f is not None and f.healable is False)
os.remove(os.path.join(NM, "enp6s0.nmconnection"))

profile("nomac.nmconnection", "legacy", "802-3-ethernet", ifname="enp3s0")
f = c.detect()
check("flags missing name with no MAC pin", f is not None and "'legacy' wants enp3s0" in f.detail)

print("PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
