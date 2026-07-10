#!/bin/bash
# schema-init Installer and Bootstrapper
# Safely configures schema-init as PID 1 and sets up basic services.
# Run as root to install binaries and create config directories.

set -e

# Color helpers
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}====================================================${NC}"
echo -e "${GREEN}       schema-init PID 1 Installer & Bootstrapper   ${NC}"
echo -e "${GREEN}====================================================${NC}"

# Check for root privileges
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: Please run this installer as root (sudo).${NC}"
    exit 1
fi

# --- Helper functions ---
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        echo "${ID:-unknown}"
    else
        echo "unknown"
    fi
}

detect_de() {
    if command -v plasmashell >/dev/null 2>&1; then
        echo "kde"
    elif command -v cinnamon >/dev/null 2>&1; then
        echo "cinnamon"
    else
        echo "headless"
    fi
}

install_profile() {
    local profile="$1"
    local profile_dir="distros/${profile}"
    if [ ! -d "$profile_dir" ]; then
        echo -e "  ${YELLOW}No profile found for '${profile}' — skipping.${NC}"
        return
    fi
    echo -e "  Installing scripts from ${profile_dir}/scripts/..."
    for f in "$profile_dir"/scripts/*; do
        [ -f "$f" ] || continue
        cp "$f" "$BIN_DIR/"
        chmod 755 "$BIN_DIR/$(basename "$f")"
        echo -e "    ${GREEN}+${NC} $(basename "$f")"
    done
    echo -e "  Installing services from ${profile_dir}/services/..."
    for f in "$profile_dir"/services/*; do
        [ -f "$f" ] || continue
        local dest="$SVC_DIR/$(basename "$f")"
        if [ ! -f "$dest" ]; then
            cp "$f" "$dest"
            echo -e "    ${GREEN}+${NC} $(basename "$f")"
        else
            echo -e "    ${YELLOW}~${NC} $(basename "$f") (already exists, skipping)"
        fi
    done
}

# RT audio: pipewire ships /etc/security/limits.d/*-pw-rlimits.conf granting
# @pipewire rtprio/memlock, but the group ships empty. Audio starts via runuser,
# whose pam_limits applies that grant against the user's groups — so the desktop
# user must be IN the pipewire group, or WirePlumber/PipeWire never get
# SCHED_FIFO and audio xruns (crackles) under load. A PID1-level RLIMIT grant
# does NOT work: pam_limits in the runuser path overrides it.
enable_rt_audio() {
    local u="$1"
    [ -n "$u" ] || return 0
    if ! getent group pipewire >/dev/null 2>&1; then
        echo -e "  ${YELLOW}pipewire group absent — skipping RT-audio grant.${NC}"
        return 0
    fi
    if id -nG "$u" 2>/dev/null | tr ' ' '\n' | grep -qx pipewire; then
        echo -e "  ${u} already in pipewire group (RT audio)."
    else
        usermod -aG pipewire "$u" && \
            echo -e "  ${GREEN}+${NC} added ${u} to pipewire group — PipeWire/WirePlumber get SCHED_FIFO."
    fi
}

# 0. Pre-flight: Python dependency check
echo -e "\n${YELLOW}[0/4] Checking Python dependencies for schema-logind...${NC}"
MISSING_PY=()
python3 -c "import dbus" 2>/dev/null     || MISSING_PY+=("python3-dbus")
python3 -c "from gi.repository import GLib" 2>/dev/null || MISSING_PY+=("python3-gobject")

if [ ${#MISSING_PY[@]} -gt 0 ]; then
    echo -e "${RED}Missing Python packages: ${MISSING_PY[*]}${NC}"
    DISTRO=$(detect_distro)
    case "$DISTRO" in
        fedora|rhel|centos|almalinux)
            echo -e "  Install with: ${YELLOW}dnf install ${MISSING_PY[*]}${NC}" ;;
        debian|ubuntu|linuxmint)
            echo -e "  Install with: ${YELLOW}apt install ${MISSING_PY[*]}${NC}" ;;
        arch|cachyos|endeavouros|manjaro)
            echo -e "  Install with: ${YELLOW}pacman -S python-dbus python-gobject${NC}" ;;
        *)
            echo -e "  Install python3-dbus and python3-gobject via your package manager." ;;
    esac
    echo -e "${RED}schema-logind.py will not start without these. Install them and re-run.${NC}"
    exit 1
fi
echo -e "${GREEN}Python dependencies OK.${NC}"

# 1. Compile schema-init statically
echo -e "\n${YELLOW}[1/4] Compiling schema-init...${NC}"
if ! command -v gcc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
    echo -e "${RED}Error: gcc and make are required for installation.${NC}"
    exit 1
fi

make clean
# Ensure we build the static binary
make
echo -e "${GREEN}Compilation successful!${NC}"

# 2. Place binary
INSTALL_DIR="/sbin"
echo -e "\n${YELLOW}[2/4] Installing binaries to ${INSTALL_DIR}...${NC}"
mkdir -p "$INSTALL_DIR"
cp -f schema-init "$INSTALL_DIR/schema-init"
cp -f schema-ctl "$INSTALL_DIR/schema-ctl"
chmod 755 "$INSTALL_DIR/schema-init"
chmod 755 "$INSTALL_DIR/schema-ctl"
echo -e "${GREEN}Installed schema-init and schema-ctl to $INSTALL_DIR.${NC}"

# 3. Create directories and populate standard services
SVC_DIR="/etc/schema-init/services"
BIN_DIR="/usr/local/bin"
echo -e "\n${YELLOW}[3/4] Bootstrapping standard system services in ${SVC_DIR}...${NC}"
mkdir -p "$SVC_DIR"
mkdir -p "$BIN_DIR"

# Helper to write files if they don't already exist
write_if_missing() {
    local target="$1"
    if [ ! -f "$target" ]; then
        cat > "$target"
        echo -e "  Created $target"
    else
        echo -e "  Skipped $target (already exists)"
    fi
}

# --- UDEV SERVICE ---
write_if_missing "$SVC_DIR/udev.svc" << 'EOF'
name=udev
exec=/lib/systemd/systemd-udevd
oneshot=0
critical=1
stable_secs=5
ready_path=/run/udev/control
priority=critical
EOF

# --- UDEV COLDPLUG TRIGGER SERVICE ---
write_if_missing "$BIN_DIR/schema-udev-trigger.sh" << 'EOF'
#!/bin/sh
echo "[udev-trigger] triggering coldplug driver discovery..."
/usr/bin/udevadm trigger --action=add || true
/usr/bin/udevadm settle --timeout=15 || true
exit 0
EOF
chmod 755 "$BIN_DIR/schema-udev-trigger.sh"

write_if_missing "$SVC_DIR/udev-trigger.svc" << 'EOF'
name=udev-trigger
exec=/usr/local/bin/schema-udev-trigger.sh
dep=udev
oneshot=1
critical=1
EOF

# --- DBUS SERVICE ---
write_if_missing "$BIN_DIR/schema-dbus.sh" << 'EOF'
#!/bin/sh
echo "[dbus] setting up runtime directory..."
mkdir -p /run/dbus
chmod 755 /run/dbus
chown messagebus:messagebus /run/dbus 2>/dev/null || true
exec /usr/bin/dbus-daemon --system --nofork --nopidfile
EOF
chmod 755 "$BIN_DIR/schema-dbus.sh"

write_if_missing "$SVC_DIR/dbus.svc" << 'EOF'
name=dbus
exec=/usr/local/bin/schema-dbus.sh
dep=udev-trigger
oneshot=0
critical=0
ready_path=/run/dbus/system_bus_socket
EOF

# --- SSHD SERVICE ---
write_if_missing "$BIN_DIR/schema-sshd.sh" << 'EOF'
#!/bin/sh
echo "[sshd] pre-creating privilege privilege separation directory..."
mkdir -p /run/sshd
chmod 755 /run/sshd
exec /usr/sbin/sshd -D
EOF
chmod 755 "$BIN_DIR/schema-sshd.sh"

write_if_missing "$SVC_DIR/sshd.svc" << 'EOF'
name=sshd
exec=/usr/local/bin/schema-sshd.sh
dep=dbus
oneshot=0
critical=0
EOF

# --- GETTY (LOCAL SHELL) SERVICE ---
write_if_missing "$SVC_DIR/getty.svc" << 'EOF'
name=getty
exec=/sbin/agetty
args=-o -p -- \u --noclear tty1 linux
oneshot=0
critical=1
stable_secs=2
EOF

# 3b. Detect DE and install matching distro profile
DISTRO=$(detect_distro)
DE=$(detect_de)
PROFILE=""

echo -e "\n${YELLOW}[3b/4] Detecting desktop environment...${NC}"
echo -e "  Distro: ${DISTRO}  |  DE: ${DE}"

case "${DISTRO}+${DE}" in
    fedora+kde|rhel+kde|centos+kde|almalinux+kde)
        PROFILE="fedora-kde" ;;
    fedora+cinnamon|rhel+cinnamon)
        PROFILE="fedora-cinnamon" ;;
    debian+cinnamon|ubuntu+cinnamon|linuxmint+cinnamon)
        PROFILE="debian-cinnamon" ;;
    *+headless)
        echo -e "  ${YELLOW}No desktop environment detected — installing core services only.${NC}"
        echo -e "  If you have a DE, install it first, then re-run setup.sh." ;;
    *)
        echo -e "  ${YELLOW}No matching profile for ${DISTRO}+${DE}.${NC}" ;;
esac

if [ -n "$PROFILE" ]; then
    echo -e "  ${GREEN}Matched profile: ${PROFILE}${NC}"
    read -r -p "  Install '${PROFILE}' desktop profile? [Y/n] " ans
    ans="${ans:-Y}"
    if [[ "$ans" =~ ^[Yy]$ ]]; then
        install_profile "$PROFILE"
        # RT audio for the desktop user (profiles ship pipewire/wireplumber)
        RT_USER=""
        [ -r /etc/schema-init/user.conf ] && RT_USER=$(. /etc/schema-init/user.conf 2>/dev/null; echo "$SCHEMA_USER")
        RT_USER="${RT_USER:-$SUDO_USER}"
        enable_rt_audio "$RT_USER"
        echo -e "${GREEN}Profile '${PROFILE}' installed.${NC}"
    else
        echo -e "  Skipped profile install."
    fi
fi

# 4. Provide boot configuration instructions
echo -e "\n${YELLOW}[4/4] Installation Complete! Bootloader Instructions:${NC}"
echo -e "=================================================================="

# Check if Raspberry Pi
if [ -f /boot/firmware/cmdline.txt ] || [ -f /boot/cmdline.txt ]; then
    if [ -f /boot/firmware/cmdline.txt ]; then
        PI_BOOT="/boot/firmware/cmdline.txt"
    else
        PI_BOOT="/boot/cmdline.txt"
    fi
    echo -e "${GREEN}Raspberry Pi environment detected!${NC}"
    echo -e "To boot into schema-init, edit $PI_BOOT and append:"
    echo -e "  ${YELLOW}init=/sbin/schema-init${NC}"
    echo -e "to the end of the single line of boot arguments."
    echo -e "To revert, mount the SD card on another machine and remove that argument."
else
    echo -e "${GREEN}x86_64/GRUB environment detected.${NC}"
    echo -e "For safety, ${RED}do NOT overwrite your default boot entry.${NC}"
    echo -e "A safe fallback GRUB entry will be generated for you.\n"

    # Detect running kernel and initrd
    KVER=$(uname -r)
    KPATH=""
    INITRD_PATH=""
    for k in "/boot/vmlinuz-${KVER}" "/boot/vmlinuz"; do
        [ -f "$k" ] && KPATH="$k" && break
    done
    for i in "/boot/initramfs-${KVER}.img" "/boot/initrd.img-${KVER}" "/boot/initrd-${KVER}.img"; do
        [ -f "$i" ] && INITRD_PATH="$i" && break
    done

    # Detect root device and fs type
    ROOT_DEV=$(findmnt -n -o SOURCE /)
    ROOT_UUID=$(blkid -s UUID -o value "$ROOT_DEV" 2>/dev/null || echo "")
    ROOT_OPTS="ro quiet"

    if [ -n "$KPATH" ] && [ -n "$INITRD_PATH" ] && [ -n "$ROOT_UUID" ]; then
        GRUB_ENTRY="menuentry 'schema-init (fallback)' {
    load_video
    insmod gzio
    insmod part_gpt
    insmod ext2
    search --no-floppy --fs-uuid --set=root ${ROOT_UUID}
    linux   ${KPATH} root=UUID=${ROOT_UUID} ${ROOT_OPTS} init=/sbin/schema-init
    initrd  ${INITRD_PATH}
}"
        CUSTOM="/etc/grub.d/40_custom"
        echo -e "Generated GRUB entry:\n"
        echo -e "${YELLOW}${GRUB_ENTRY}${NC}\n"
        read -r -p "Write this entry to ${CUSTOM} and run grub-mkconfig? [Y/n] " ans
        ans="${ans:-Y}"
        if [[ "$ans" =~ ^[Yy]$ ]]; then
            echo "" >> "$CUSTOM"
            echo "$GRUB_ENTRY" >> "$CUSTOM"
            if command -v grub2-mkconfig >/dev/null 2>&1; then
                grub2-mkconfig -o /boot/grub2/grub.cfg
            elif command -v update-grub >/dev/null 2>&1; then
                update-grub
            else
                echo -e "${YELLOW}Run grub-mkconfig manually to apply.${NC}"
            fi
            echo -e "${GREEN}GRUB entry written. Reboot and select 'schema-init (fallback)' to test.${NC}"
        else
            echo -e "  Copy the entry above into /etc/grub.d/40_custom manually, then run grub2-mkconfig."
        fi
    else
        echo -e "${YELLOW}Could not auto-detect kernel/initrd/root — add the GRUB entry manually.${NC}"
        echo -e "  Add 'init=/sbin/schema-init' to your kernel line in /etc/grub.d/40_custom."
        echo -e "  Then run: grub2-mkconfig -o /boot/grub2/grub.cfg"
    fi
fi
echo -e "=================================================================="

# 5. Post-install gotcha checklist
echo -e "\n${YELLOW}[Post-install] Known gotchas — read before rebooting:${NC}"
echo -e "=================================================================="
cat << 'GOTCHAS'
  1. NetworkManager: if your NM service uses ready_path, point it to
     /run/NetworkManager/resolv.conf — NOT a pid file. NM creates that
     file when the network is actually up.

  2. PipeWire/WirePlumber wrapper scripts MUST export:
       XDG_RUNTIME_DIR=/run/user/1000
     before exec'ing the binary, or PipeWire can't create its socket.

  3. KDE Plasma: your session wrapper MUST set:
       PLASMA_USE_SYSTEMD_SCOPE=0
     or plasmashell will try to contact systemd --user and hang.

  4. kwin_wayland: do NOT pass WAYLAND_DISPLAY in the environment when
     launching it as the compositor — it will use nested mode instead of
     taking the seat directly.

  5. seatd: it only detects seat devices on cold boot. If you add a
     service that depends on seatd after boot (live-add), it will stay
     DORMANT. Always reboot to test seat-dependent services.

  6. Avoid 'args=' in .svc files for multi-word arguments — use a
     wrapper script instead. The args= field has a leading-space bug
     that silently corrupts argv[0].

  7. If you have both udhcpc and NetworkManager configured for the same
     interface, they will fight. Disable one — pick NM and remove udhcpc.

  8. chronyd depends on network being up. If NM never signals ready_path,
     chronyd will hang at startup waiting for its dep to be satisfied.

  9. Audio: two things beyond XDG_RUNTIME_DIR (see #2).
     - RT: the desktop user must be in the 'pipewire' group or PipeWire never
       gets SCHED_FIFO and audio crackles under load. setup.sh adds them when
       a desktop profile is installed. (A PID1 RLIMIT grant won't work —
       runuser's pam_limits overrides it; group membership is the lever.)
     - Session modules (mpris pause, device reservation): WirePlumber needs the
       graphical-session bus env, which wireplumber-run.sh harvests from a live
       Plasma/kwin process (no systemd --user to provide it).
GOTCHAS
echo -e "==================================================================\n"
echo -e "${GREEN}Done. Reboot and select the schema-init GRUB entry to test.${NC}"
echo -e "Logs: ${YELLOW}/var/log/schema-init/<service>.log${NC}"
