# schema-init — Raspberry Pi Zero W

Runs schema-init as PID 1 on a Raspberry Pi Zero W (BCM2835, armv6l, 32-bit ARM). WiFi headless — no Ethernet, no HDMI required. Install, enable and boot fallback are in [the Raspberry Pi README](../raspberry-pi/README.md). Network comes up via wpa_supplicant + dhcpcd; sshd is the first usable interface.

Tested on Pi OS Trixie (Debian 13, 32-bit), kernel 6.12.75+rpt-rpi-v6.

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

## Building

The Pi Zero W is armv6l. Fedora's `arm-linux-gnu-gcc` cross-compiler does not ship an arm sysroot, so cross-compilation from Fedora is not practical. **The apt package ships `armhf` (ARMv6) binaries, so building is optional.** To build anyway, compile natively on the Pi:

```sh
# on the Pi (via SSH once it's up, or from another Pi OS machine)
sudo apt install git gcc make
git clone https://github.com/ajax80/schema-init
cd schema-init && make
```

The `armhf` Makefile target exists for completeness but requires a full arm sysroot on the build host.

## Installation

Install from the apt repo and enable with `schema-pi`: see [distros/raspberry-pi](../raspberry-pi/README.md). It also covers boot fallback; `schema-init-pi` writes the boot log to `/var/log/schema-init/boot.log`, so the old `schema-init-log` wrapper is no longer needed. The Zero W service files and wrapper scripts are not in the package. From this directory:

```sh
sudo mkdir -p /etc/schema-init/services
sudo cp services/* /etc/schema-init/services/
sudo cp scripts/schema-*.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/schema-*.sh
```

Then configure WiFi as above, run `sudo schema-pi enable` and reboot.

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
