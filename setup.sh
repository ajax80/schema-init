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

# 4. Provide boot configuration instructions
echo -e "\n${YELLOW}[4/4] Installation Complete! Bootloader Instructions:${NC}"
echo -e "=================================================================="

# Check if Raspberry Pi
if [ -f /boot/firmware/cmdline.txt ] || [ -f /boot/cmdline.txt ]; then
    PI_BOOT=""
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
    echo -e "${GREEN}x86_64 or generic GRUB environment detected!${NC}"
    echo -e "For safety, ${RED}do NOT overwrite your default boot entry.${NC}"
    echo -e "Instead, add a secondary, fallback boot choice to your GRUB menu:"
    echo -e "1. Edit /etc/grub.d/40_custom and add a menuentry pointing to your kernel"
    echo -e "   adding ${YELLOW}init=/sbin/schema-init${NC} to the linux argument line."
    echo -e "2. Run ${YELLOW}sudo update-grub${NC} (or grub2-mkconfig) to apply."
    echo -e "This way, if schema-init fails to boot, you can reboot and select your normal OS."
fi
echo -e "=================================================================="
