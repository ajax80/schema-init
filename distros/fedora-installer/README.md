# schema installer ISO — the "dad-proof" build

> **STATUS: EXPERIMENTAL — not yet validated end to end.**
> The ISO builds and is correctly wired (verified), but has **not** been booted
> through a real install yet. These are developer build notes, *not* a user
> promise. It stays out of the top-level README Quickstart until it has survived
> the full test ladder (Boxes → a real spare laptop → a true stranger's machine).

## What this is

An installer ISO that turns a stock Fedora 44 install into a schema-init system
with **no Linux knowledge required from the person installing it**. The user sees
the ordinary Fedora **Anaconda** GUI — same screens as any Fedora install — and
schema takes over silently in the background, then a friendly first-boot wizard
offers the optional schema-udev flip.

Two deliberate safety stances:

- **schema-init as PID 1 installs unconditionally** and is the safe part:
  systemd-udev still owns `/dev`, so the machine is always bootable.
- **The schema-udev flip is opt-in, gated, and self-healing.** It never runs
  unattended on unknown hardware, and a flip that breaks `/dev` **auto-rolls-back
  on the next boot** with no GUI needed (see the healthcheck seatbelt).

## Base ISO: netinst, not Live

Use **`Fedora-Everything-Netinst-x86_64-44-*.iso`**, not a Live spin.

- netinst boots *straight into Anaconda*, so `inst.ks` + `%post` run the standard,
  documented way.
- A Live ISO boots to a desktop with **no installer boot entry** — the installer
  is launched from inside the session, and a kickstart can't reliably attach.
- The installed machine still gets **KDE** ("just like blakbox") — that comes from
  the `@^kde-desktop-environment` package set in `schema.ks`, not from the ISO
  flavor.

Trade-off: netinst **needs a network at install time** (it pulls packages from the
Fedora mirrors).

## Files

| file | role |
|------|------|
| `schema.ks` | kickstart: install source, KDE `%packages`, and the schema `%post` |
| `firstboot-flip-wizard.sh` | yad/GTK guided udev-flip wizard (two-phase, novice language) |
| `schema-udev-flip-healthcheck.sh` | headless oneshot that auto-rolls-back a bad flip before the desktop |
| `build-iso.sh` | `mkksiso` wrapper — builds binaries, stages payload, injects the kickstart |

## Build

```sh
# prereq (build host):
sudo dnf install -y lorax          # provides mkksiso

# build (arg 1 = base netinst ISO, arg 2 = output):
./build-iso.sh ~/Downloads/Fedora-Everything-Netinst-x86_64-44-1.7.iso \
               ~/schema-netinst44-installer.iso
```

`build-iso.sh` rebuilds the schema binaries from the current branch, stages them
plus the flip tooling and generic service rail into a payload, and runs
`mkksiso --ks schema.ks --add <payload>` (the `mkksiso` step needs sudo for
`mkefiboot`; output ownership is handed back to you).

### Verify the wiring (optional)

```sh
xorriso -indev ~/schema-netinst44-installer.iso -ls /schema/        # payload present
# inst.ks appended to every boot entry, UEFI + BIOS:
sudo mount -o loop,ro ~/schema-netinst44-installer.iso /mnt/x
grep inst.ks /mnt/x/EFI/BOOT/grub.cfg /mnt/x/boot/grub2/grub.cfg ; sudo umount /mnt/x
```

## Write to USB

```sh
sudo dd if=~/schema-netinst44-installer.iso of=/dev/sdX bs=4M status=progress oflag=direct; sync
```

Confirm `/dev/sdX` is the USB stick, not a disk — `dd` does not ask twice.

## Test ladder (do these in order — do not skip to hardware)

1. **Boxes / KVM VM** — ~25 GB disk, 4 GB+ RAM, networking on. Watch:
   - boots straight into Anaconda and picks up the kickstart (no manual ks prompt);
   - `%post` runs — `/root/schema-ks-post.log` in the installed system is the receipt;
     a `schema-<ver>.conf` BLS entry exists in `/boot/loader/entries` alongside the
     pristine stock entry, and `grub2-editenv - list` shows `saved_entry=schema-<ver>`;
     `gen-services` produced a non-empty rail;
   - reboots into **schema-init as PID 1** (the schema entry is the default) with a
     working KDE desktop; the stock **Fedora** entry is still in the menu as a fallback;
   - `sudo schema-ctl timing` shows the per-service cost table.
2. **A real spare machine** (e.g. the Dell laptop) — same checks on metal.
3. **A true stranger's machine** — only after 1 and 2 are clean.

Only touch the **udev flip wizard** after step 1 is solid.

## Known caveats

- **Live ISO base does not work** — use netinst (see above).
- **`mkefiboot` case-collision:** some Fedora ISOs carry duplicate case-variant EFI
  files (`bootx64.efi` + `BOOTX64.EFI`); on those, `mkksiso`'s `mkefiboot` fails with
  `File exists`. The Fedora 44 **KDE Live** ISO hits this; the **netinst** ISO does
  **not**. If a future base does, the fallback is a manual `xorriso -boot_image any
  replay` repack that preserves the original boot images and only patches `grub.cfg`
  to append `inst.ks` — no EFI-image rebuild (noted in `build-iso.sh`).
- **`gen-services` in `%post`** reads systemd units offline (no running PID 1 in the
  installer chroot). Usually fine; the generic rail is the fallback if it comes up empty.
- **First-boot flip is reboot-gated** — the wizard arms + reboots + verifies; it is
  not a single-window action. The GUI is the happy path; the headless healthcheck is
  the seatbelt for a flip that breaks boot.
- **Two entries per kernel in the boot menu** — a stock `Fedora Linux (...)` entry
  (boots systemd, the fallback) and a `... (schema-init)` entry (the saved default).
  This is intentional: the stock entry is a novice's escape hatch if schema ever fails
  to boot. The `kernel-install` plugin (`/etc/kernel/install.d/99-schema-init.install`)
  regenerates the schema entry on every `dnf` kernel update and repoints the default at
  it, gated on the `/etc/schema-init/boot-default` marker; per-box extra kernel args go
  in `/etc/schema-init/kernel-cmdline.d/*.conf` (the installer seeds `enforcing=0` there).
