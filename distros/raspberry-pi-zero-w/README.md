# schema-init — Raspberry Pi Zero W

Runs schema-init as PID 1 on a Raspberry Pi Zero W (BCM2835, armv6l, 32-bit ARM). WiFi headless — no Ethernet, no HDMI required. Network comes up via wpa_supplicant + dhcpcd; sshd is the first usable interface.

Tested on Pi OS Trixie (Debian Bookworm, 32-bit), kernel 6.12.75+rpt-rpi-v6.

## What this replaces

- systemd (PID 1) → schema-init
- systemd-networkd / dhcpd / NM → dhcpcd (foreground, wlan0 only)
- systemd-udev-trigger.service → udev-trigger oneshot service

## Service chain

```
udev (daemon)
  └─ udev-trigger (oneshot — coldplug trigger + settle)
       └─ wpa-supplicant (dep: udev-trigger + dbus)
            └─ dhcpcd (foreground, wlan0)
                  └─ sshd
dbus (daemon, parallel with udev)
```

## Key gotchas — hard-won from first ARM bare-metal deploy

**1. udev coldplug trigger is required.**
systemd runs `udev-trigger.service` at boot to replay device uevents for hardware present at power-on. Without it, `udevd` starts but the BCM2835's SDIO WiFi device never gets its uevent and brcmfmac firmware never loads — `wlan0` does not exist. The `udev-trigger` oneshot service (`udevadm trigger --action=add && udevadm settle`) replaces this.

**2. wpa_supplicant on Pi OS Trixie has D-Bus compiled as mandatory.**
Even in config-file mode (no `-u` flag), wpa_supplicant checks for `/run/dbus/system_bus_socket` at startup and exits if absent. `dbus` must be a dep of `wpa-supplicant`, not optional.

**3. dbus-daemon drops privileges to `messagebus` immediately.**
`/run/dbus` must be `chown messagebus:messagebus` before dbus-daemon starts, or it cannot write its pid file. `schema-dbus.sh` handles this.

**4. brcmfmac requires a regulatory country code.**
Without `country=` in wpa_supplicant.conf, the Broadcom driver soft-blocks the radio via rfkill. wpa_supplicant initializes successfully but the radio is dead — dhcpcd gets no carrier. Add `country=US` (or your country) to the config. `schema-wpa.sh` also calls `rfkill unblock wifi` as a belt-and-suspenders measure.

**5. dhcpcd must run in the foreground.**
Default `dhcpcd` forks after lease acquisition. schema-init detects the parent exit, kills the cgroup, and terminates dhcpcd. Pass `-B` (`--nobackground`) to keep it supervised. `schema-dhcpcd.sh` does this.

**6. sshd requires /run/sshd for privilege separation.**
`/run` is a fresh tmpfs under schema-init. sshd on Debian crashes if `/run/sshd` is missing. `schema-sshd.sh` creates it before exec.

**7. wlan0 may not exist immediately after udev-trigger.**
The `schema-wpa.sh` wrapper polls `ip link show wlan0` for up to 30 seconds before calling wpa_supplicant, as a fallback in case firmware load is slow.

## WiFi configuration

Create `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`:

```
ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
update_config=1
country=US
network={
    ssid="YourSSID"
    psk="YourPassphrase"
}
```

The PSK must be quoted (passphrase format). An unquoted PSK is interpreted as a 64-character hex string and will fail.

## Persistent logging

schema-init redirects per-service logs to `/run/log/schema-init/` (tmpfs — lost on power cut). A wrapper is needed to persist boot logs. Install `/sbin/schema-init-log`:

```sh
#!/bin/sh
mount -o remount,rw / 2>/dev/null
mkdir -p /var/log/schema-init
exec /sbin/schema-init >/var/log/schema-init/boot.log 2>&1
```

Then set `init=/sbin/schema-init-log` in `/boot/firmware/cmdline.txt`. The wrapper scripts in this distro also redirect their per-service output to `/var/log/schema-init/` (wpa.log, dbus.log, dhcpcd.log).

## Building

The Pi Zero W is armv6l. Fedora's `arm-linux-gnu-gcc` cross-compiler does not ship an arm sysroot, so cross-compilation from Fedora is not practical. **Compile natively on the Pi:**

```sh
# on the Pi (via SSH once it's up, or from another Pi OS machine)
sudo apt install git gcc make
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

The `armhf` Makefile target exists for completeness but requires a full arm sysroot on the build host.

## Installation

### 1. Build binary on the Pi (see Building above)

### 2. Install schema-init

```sh
sudo cp schema-init /sbin/schema-init
sudo cp schema-ctl /usr/local/bin/schema-ctl
sudo mkdir -p /etc/schema-init/services
```

### 3. Install service files

```sh
sudo cp services/* /etc/schema-init/services/
```

### 4. Install wrapper scripts

```sh
sudo cp scripts/schema-dbus.sh /usr/local/bin/schema-dbus.sh
sudo cp scripts/schema-udev-trigger.sh /usr/local/bin/schema-udev-trigger.sh
sudo cp scripts/schema-wpa.sh /usr/local/bin/schema-wpa.sh
sudo cp scripts/schema-dhcpcd.sh /usr/local/bin/schema-dhcpcd.sh
sudo cp scripts/schema-sshd.sh /usr/local/bin/schema-sshd.sh
sudo chmod +x /usr/local/bin/schema-*.sh
```

### 5. Install logging wrapper

```sh
sudo cp scripts/schema-init-log /sbin/schema-init-log
sudo chmod +x /sbin/schema-init-log
```

### 6. Configure WiFi

```sh
sudo nano /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
# add country=, ssid=, psk= — see WiFi configuration above
```

### 7. Set init in cmdline.txt

Edit `/boot/firmware/cmdline.txt` and add `init=/sbin/schema-init-log` to the kernel line. Keep it on one line.

### 8. Boot

Power cycle. After ~50 seconds the Pi will be reachable via SSH at its DHCP address.

## Boot timeline (measured)

```
kernel → PID 1:          ~14s
udev ready:              +1s
udev-trigger (settle):   +13s
dbus ready:              parallel with udev-trigger
wpa-supplicant ready:    +2s  (socket appears)
dhcpcd lease:            +30s (stable_secs)
sshd ready:              +3s
```

total kernel → SSH available: ~50s
