schema-init on a Pi Zero W: ARM bare-metal, WiFi only, SSH in 50 seconds

schema-init is a statically linked C PID 1 (~960 KB RSS) built around a weight-state machine. Previous posts covered desktop deploys on x86. This one is the first ARM bare-metal target.

**Hardware:** Raspberry Pi Zero W — BCM2835, armv6l, 32-bit, single core 1GHz. WiFi only. No Ethernet port. No HDMI adapter. Flying completely blind via persistent logs written to the SD card.

**Goal:** schema-init as PID 1, WiFi connected, sshd up. Confirm with `ps -p 1 -o comm=`.

**Result:**

```
Linux (none) 6.12.75+rpt-rpi-v6 armv6l GNU/Linux
PID 1: schema-init
```

SSH accessible ~50 seconds from cold boot.

---

**Service chain:**

```
udev (daemon)
  └─ udev-trigger (oneshot — coldplug trigger + settle)
       └─ wpa-supplicant (dep: udev-trigger + dbus)
            └─ dhcpcd (-B, foreground, wlan0)
                  └─ sshd
dbus (parallel with udev)
```

No NetworkManager. No D-Bus mode wpa_supplicant. Traditional headless Pi stack.

---

**What actually broke, in order:**

**1. PSK quoting.** `psk=mypassword` fails — wpa_supplicant parses unquoted values as 64-char hex. Needs `psk="mypassword"`. Caught from wpa.log on the SD card.

**2. wlan0 never existed.** systemd normally runs `udev-trigger.service` to replay device uevents for hardware present at power-on. Without it, udevd starts but the BCM2835's SDIO WiFi device never gets its uevent replayed — brcmfmac firmware never loads, wlan0 doesn't appear. Fixed: dedicated `udev-trigger` oneshot service runs `udevadm trigger --action=add && udevadm settle --timeout=15` before wpa-supplicant is allowed to start.

**3. rfkill soft block.** wpa.log showed `rfkill: WLAN soft blocked`. brcmfmac blocks the radio until a regulatory country code is set. No country code = no radio, regardless of association state. Fix: `country=US` in wpa_supplicant.conf, plus `rfkill unblock wifi` in the startup wrapper.

**4. dbus mandatory.** Pi OS Trixie's wpa_supplicant is compiled with D-Bus required even in config-file mode. It checks for `/run/dbus/system_bus_socket` at startup and exits if absent. dbus must be a dep of wpa-supplicant, not optional.

**5. dhcpcd daemonizes.** Default dhcpcd forks after lease acquisition. schema-init sees the parent exit, kills the cgroup, RECOVERY arc fires 5 times in 2 seconds, DORMANT. Fix: `-B` flag keeps it foreground.

**6. /run/sshd missing.** `/run` is fresh tmpfs under schema-init. sshd on Debian requires the privilege separation directory at `/run/sshd`. Wrapper script creates it before exec.

---

**Build:** Fedora's `arm-linux-gnu-gcc` ships without an arm sysroot. Compiled natively on the Pi via SSH from a working Pi OS boot. `make` on the Pi, scp to card.

**Logs:** Per-service stdout/stderr goes to `/run/log/schema-init/` (tmpfs). Wrapper scripts redirect to `/var/log/schema-init/` which survives the power cycle. That's how you debug a headless box with no serial adapter — mount the SD card and read the files.

**distros/raspberry-pi-zero-w/** is in the repo now with all six service files and five wrapper scripts.

GitHub: https://github.com/ajax80/schema-init — AGPL-3.0, commercial license available.

---

## Comment — Pi Zero W performance numbers (add under turbostat comment)

Pi Zero W numbers — schema-init on armv6l, 512 MB, WiFi only:

15 minutes uptime, SSH only, no desktop:

```
PID 1 RSS:     684 KB
RAM used:       74 MB / 427 MB available
Swap:            0
```

Typical Pi Zero W idle under Pi OS + systemd: 130–180 MB RAM, swap active, PID 1 at 8–12 MB, journald + resolved + networkd + logind all running in the background.

On a 512 MB machine the headroom difference is real. No swap pressure. No journal daemon. No resolver daemon. Just udev, dbus, wpa_supplicant, dhcpcd, sshd, and PID 1 at 684 KB.

`distros/raspberry-pi-zero-w/` is in the repo — full service chain, wrapper scripts, and the gotchas (rfkill country code, D-Bus mandatory in wpa_supplicant, udev coldplug trigger) are documented.
