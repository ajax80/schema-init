# schema-init — Daedalus Supervisor Node (Slot 48)

The Supervisor is the brain of the Ungulate Leg. It runs on a Pi 4 or CM4 (aarch64, 4GB+ RAM) and is responsible for voice command intake, on-device AI inference, and coordination of the 48 joint nodes (slots 0–47).

The joint nodes (Pi Zero W 2) run the `raspberry-pi-zero-w` distro profile. This profile is for slot 48 only.

## What this provides

- Voice command intake via USB microphone → whisper.cpp (local, offline)
- On-device inference via llama.cpp server or remote offload to Blakbox (192.168.8.102:11434) when in range
- SSH access for manual control and debugging
- Coordination daemon (`supervisor-agent`) that translates voice commands to joint node instructions

## Service chain

```
udev
  └─ udev-trigger
       └─ wpa-supplicant (dep: udev-trigger + dbus)
            └─ dhcpcd
                  └─ sshd
                  └─ pipewire (waits for USB audio device)
                       └─ whisper (dep: pipewire socket ready)
                            └─ supervisor-agent (dep: whisper + dhcpcd)
dbus (parallel with udev)
```

`supervisor-agent` is `critical=1` — if it dies the leg has no brain and schema-init will attempt recovery.

## Hardware

- Pi 4 (4GB minimum) or Compute Module 4
- USB audio adapter (any USB Audio Class device — plug and play)
- USB microphone or headset with mic
- WiFi or Ethernet to the same LAN as the joint nodes and Blakbox

## User account

Scripts assume a `daedalus` user (uid 1000) for PipeWire. Create it:

```sh
sudo useradd -m -u 1000 -s /bin/sh daedalus
sudo usermod -aG audio daedalus
```

## Prerequisites

### whisper.cpp

Build from source on the Pi 4:

```sh
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp && make -j4
sudo cp build/bin/whisper-server /usr/local/bin/
```

Download the small English model (~150MB):

```sh
sudo mkdir -p /usr/local/share/whisper
sudo bash models/download-ggml-model.sh small.en /usr/local/share/whisper/
```

### llama.cpp (optional — for on-device inference)

If running inference locally instead of offloading to Blakbox:

```sh
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp && make -j4
sudo cp llama-server /usr/local/bin/
```

A 1B–3B parameter model (TinyLlama, Phi-2) fits comfortably in 4GB RAM.
When Blakbox is on the LAN, `supervisor-agent` offloads to Ollama at `192.168.8.102:11434` instead.

### Node map

Create `/etc/daedalus/nodes.conf` listing each joint node's IP and slot assignment:

```
# slot  ip              role
0       192.168.8.10    hip-left-0
1       192.168.8.11    hip-left-1
...
48      192.168.8.58    supervisor
```

## Installation

### 1. Build and install schema-init (native on Pi 4)

```sh
sudo apt install git gcc make
git clone https://github.com/ajax80/schema-init
cd schema-init && make
sudo cp schema-init /sbin/schema-init
sudo cp schema-ctl /usr/local/bin/schema-ctl
sudo mkdir -p /etc/schema-init/services
```

### 2. Install service files

```sh
sudo cp distros/supervisor/services/* /etc/schema-init/services/
```

### 3. Install scripts

```sh
sudo cp distros/supervisor/scripts/*.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/schema-*.sh
```

### 4. Configure WiFi

```sh
sudo nano /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
```

```
ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
update_config=1
country=US
network={
    ssid="YourSSID"
    psk="YourPassphrase"
}
```

### 5. Set init

Edit `/boot/firmware/cmdline.txt` — add `init=/sbin/schema-init` to the kernel line.

### 6. Boot

Power cycle. After ~50s the Supervisor will be reachable via SSH. Once whisper is up (~30s more), the `supervisor-agent` will start listening for voice commands.

## Key differences from joint nodes

| | Joint node (Pi Zero W 2) | Supervisor (Pi 4) |
|---|---|---|
| Arch | armv6l (32-bit) | aarch64 (64-bit) |
| RAM | 512MB | 4GB+ |
| Audio | none | USB mic + pipewire |
| Inference | none | whisper + llama.cpp / Blakbox offload |
| Role | actuator control | voice intake + coordination |
| critical service | none | supervisor-agent |
