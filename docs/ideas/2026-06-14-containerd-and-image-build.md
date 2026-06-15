# Future Increments — Notes (2026-06-14)

Two transferable techniques spotted in `Shreyas0047/veilbox` (r/embeddedlinux). Both are
*additive* to schema-init — develop in the morning. Captured here with the decisions
already pinned so we don't re-derive.

---

## Idea A — Container support: `containerd.svc`

schema-init is already a cgroup-v2 manager (`service.h`: `cpu_limit_pct`→cpu.max,
`mem_limit_mb`→memory.max, priority→cpu.weight, and we hand-wired io.max 2026-06-14). So
containers aren't a rewrite — they're a supervised daemon + correct cgroup driver.

**The one critical decision (the no-systemd gotcha):**
containerd/runc must use the **`cgroupfs`** cgroup driver, NOT the `systemd` driver. The
systemd driver creates containers as transient scopes over D-Bus and dies without systemd.
Set in `/etc/containerd/config.toml`:
```toml
[plugins."io.containerd.grpc.v1.cri".containerd.runtimes.runc.options]
  SystemdCgroup = false
```

**Plan:**
1. `containerd.svc` under `/etc/schema-init/services/` — long-running daemon, dep on the
   cgroup2 mount + network. Probably `priority=standard`, `no_excise` off.
2. Ship the `SystemdCgroup = false` config (a distro file, like our other dropped configs).
3. Ensure cgroup2 is mounted with controllers delegated (`cgroup.subtree_control`) at the
   path containerd builds under. Verify our boot already enables the controllers containerd
   needs (cpu, memory, io, pids).
4. Smoke test: `ctr run` a tiny image (busybox) under schema-init in the vmtest harness or
   on a spare box. Confirm the container's cgroup lands under the unified hierarchy and
   gets cleaned up on exit.

**Open questions:** does containerd's own cgroup want to be a schema-init service cgroup
(nested), or a sibling? Decide delegation boundary. Networking = start with CNI bridge or
host net for the first cut.

---

## Idea B — Bootable disk-image build pipeline (no root, no `grub-install`)

Lets us **ship schema-init as a `dd`-able image** for Pi/embedded + anyone who doesn't want
to run `setup.sh` as root. Pairs directly with the no-initramfs/BLS work
([[project_schemakernel_no_initramfs]]).

**Why "no root":** build a disk *image file*, not a live device — file writes need no
privileges. Reproducible in userspace/CI.

**Plan:**
1. Make a sparse image file; lay GPT (or MBR) — `sgdisk` on the file, or write the table
   bytes directly.
2. Create + populate the rootfs partition (loop-mount or `mtools`/`mkfs` on an offset;
   for fully-rootless, build the fs image separately and copy in).
3. Embed GRUB by hand (the veilbox trick): `grub-mkimage` → `core.img`; write the 446-byte
   `boot.img` to sector 0 and embed `core.img` in the MBR gap (sectors 1–2047) for BIOS, or
   the BIOS-boot partition for GPT. Pure `seek`+`write` at offsets — scriptable in Python/sh.
4. Bake the cmdline in the BLS/grub.cfg: `root=PARTUUID=… rdinit=/sbin/schema-init`
   (PARTUUID, not UUID — the no-initramfs gotcha).
5. Output: `schema-init-<arch>.img` artifact. Test-boot it in QEMU (`-drive`), confirm it
   reaches the schema-init rail.

**Open questions:** UEFI path too (ESP + GRUB EFI binary) or BIOS-only first? Likely do
BIOS first (simplest, matches the veilbox approach), add UEFI after.

---

Both are "stock techniques, confirmed to coexist on a hand-rolled system." Lowest-risk
start = Idea A (containerd) since the cgroup machinery already exists. Develop in the AM.
