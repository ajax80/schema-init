# Static device nodes at boot — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** PID 1 creates the kernel's `modules.devname` static nodes on first boot and applies `static-nodes-permissions.conf`, so `/dev/net/tun`, `/dev/uinput`, `/dev/vhost-net` … exist before any module is loaded.

**Architecture:** One new header `static_nodes.h` (static inline, no static state — `check_tu_statics.sh`), every entry point taking a `root` prefix so tests run in a tmpdir. `init.c` calls one function from its first-boot block between `mount_pseudo()` and `load_configured_modules()`.

**Tech Stack:** C99 + `_GNU_SOURCE`, static glibc, `make test` asserts-style C tests, QEMU vmtest.

**Spec:** `docs/superpowers/specs/2026-10-30-static-dev-nodes-design.md` (approved 2026-10-30, Greg-reviewed). Read it first.

## Global Constraints

- No fork/exec, no kmod dependency; missing input file = no-op.
- Boot only: inside `if (reexec_fd < 0)` in `init.c`.
- Never change an existing node's type/devno; `EEXIST` on mknod silent.
- New nodes: `(type=='c' ? S_IFCHR : S_IFBLK) | 0600`; parent dirs 0755, never the leaf.
- Reject devnames with `..` or a leading `/`; reject perm paths not under `/dev/` or containing `..`.
- Perm file search: `/etc/tmpfiles.d/` → `/run/tmpfiles.d/` → `/usr/local/lib/tmpfiles.d/` → `/usr/lib/tmpfiles.d/`, first found wins.
- Perms: only `z` lines; `-` = unchanged; mode plain octal else skipped per field; all-digit owner/group = numeric id; apply via `O_PATH|O_NOFOLLOW|O_CLOEXEC` + `fstat` char/block only + `/proc/self/fd/N`; `ENOENT` silent.
- Log: `[schema-init] static nodes: created N (a b c)`; errors `[schema-init] static node <path>: <strerror>`.

## Review Focus

1. A `modules.devname` line with a block device (`b`) — must mknod S_IFBLK, not char. → Task 1 test `devname_block`.
2. Perm line naming a path that is a symlink or regular file in `/dev` — must be left untouched. → Task 2 test `perms_skip_non_device` (regular file; runs unprivileged).
3. Perm line for a missing node — no error, nothing created. → Task 2 test `perms_enoent_silent`.
4. Override in `/etc/tmpfiles.d` with the same name — `/usr/lib` file must be ignored entirely. → Task 2 test `perms_precedence`.
5. Unknown group name on a line that also has a mode — mode still applied, group left. → Task 2 test `perms_unknown_group_keeps_mode` (root-gated).

---

### Task 1: devname parse + node creation

**Files:**
- Create: `static_nodes.h`
- Create: `tests/test_static_nodes.c`
- Modify: `Makefile` (test target, beside the `test_disk_links.c` line ~313)

**Interfaces:**
- Produces:
  - `struct sn_node { char devname[128]; char type; unsigned maj, min; };`
  - `int sn_parse_devname_line(const char *line, struct sn_node *out)` → 1 parsed, 0 skip (comment/blank/malformed/rejected path).
  - `int sn_create_nodes(const char *root, const char *devname_file, char *created, size_t created_sz)` → count created; `created` gets space-separated devnames. `root` is prepended to `/dev/...` ("" in PID 1).

- [ ] **Step 1: Write the failing tests** in `tests/test_static_nodes.c`:
  - `devname_char`: `"tun net/tun c10:200"` → 1, devname `net/tun`, type `c`, 10, 200.
  - `devname_block`: `"loop loop0 b7:0"` → 1, type `b`, 7, 0.
  - `devname_skip`: `"# Device nodes…"`, `""`, `"tun net/tun"`, `"tun net/tun x10:200"`, `"x ../etc/shadow c1:1"`, `"x /etc/shadow c1:1"` → all 0.
  - `create_parents_only` (always): tmp root with `dev/`; file with `tun net/tun c10:200`; call `sn_create_nodes`; assert `<root>/dev/net` is a dir 0755 and `<root>/dev/net/tun` is NOT a directory.
  - `create_mknod` (only if `geteuid()==0`): same → returns 1, `created == "net/tun"`, `stat` gives `S_ISCHR`, `major==10 minor==200`, mode `0600`; second call returns 0 (EEXIST silent); block line gives `S_ISBLK`.
  - Missing devname file → returns 0.
- [ ] **Step 2:** add `$(CC) $(CFLAGS) tests/test_static_nodes.c -o /tmp/schema-test-staticnodes && /tmp/schema-test-staticnodes` to `test:`; run `make test` → compile FAIL (no header).
- [ ] **Step 3: Implement** in `static_nodes.h` (includes `<sys/sysmacros.h>`, `<sys/stat.h>`, `<errno.h>`). Parse with `sscanf(line, "%*s %127s %c%u:%u", …)` == 4 then type ∈ {c,b} and path checks. Errors other than EEXIST print the spec's error line.
- [ ] **Step 4:** `make test` → static_nodes tests PASS (as user; `sudo /tmp/schema-test-staticnodes` → mknod cases PASS).
- [ ] **Step 5: Commit** `static_nodes: parse modules.devname and create nodes`.

### Task 2: permissions pass

**Files:** Modify `static_nodes.h`, `tests/test_static_nodes.c`.

**Interfaces:**
- Consumes: Task 1 header.
- Produces:
  - `struct sn_perm { char path[128]; int has_mode; mode_t mode; char user[64]; char group[64]; };` (`user`/`group` empty = `-`)
  - `int sn_parse_perm_line(const char *line, struct sn_perm *out)` → 1 / 0.
  - `int sn_find_perm_file(const char *root, char *out, size_t sz)` → 0 found (`out` = full path), -1 none.
  - `void sn_apply_perms(const char *root, const char *perm_file)`.

- [ ] **Step 1: Write the failing tests:**
  - `perm_parse`: `"z /dev/net/tun 0666 - - -"` → 1, mode 0666, user/group empty. `"z /dev/kvm 0666 - kvm -"` → group `kvm`. `"z /dev/x 06x6 - audio -"` → 1, `has_mode==0`, group `audio`.
  - `perm_parse_skip`: `"d /dev/foo 0755 - - -"`, `"z /etc/shadow 0666 - - -"`, `"z /dev/../etc/shadow 0666 - - -"`, `"# z /dev/a 0666"`, `""` → 0.
  - `perms_precedence`: root with both `etc/tmpfiles.d/static-nodes-permissions.conf` and `usr/lib/tmpfiles.d/…` → `sn_find_perm_file` returns the `/etc` one; only `usr/lib` → that one; neither → -1.
  - `perms_enoent_silent`: perm file lists `/dev/kvm`, root has none → no crash, `<root>/dev/kvm` still absent.
  - `perms_skip_non_device`: regular file `<root>/dev/fuse` mode 0600, line `z /dev/fuse 0666 - - -` → still 0600.
  - Root-gated (`geteuid()==0`): mknod `<root>/dev/net/tun` c10:200 0600, line `0666` → mode 0666. `perms_unknown_group_keeps_mode`: line `z /dev/net/tun 0660 - nosuchgroup -` → mode 0660, gid unchanged. Numeric: `z /dev/net/tun - - 5 -` → gid 5.
- [ ] **Step 2:** `make test` → FAIL.
- [ ] **Step 3: Implement.** tmpfiles columns are `type path mode user group …`. Copy the fd discipline from `schema-udev.h:263-289` (`node_apply_perms`) but with tmpfiles semantics (`-` keeps current uid/gid/mode). Numeric ids: `strtoul` with full-string check as `service.c:2502`. Unknown name logs `[schema-init] static node <path>: unknown user|group '<name>'`.
- [ ] **Step 4:** `make test` PASS; `sudo /tmp/schema-test-staticnodes` PASS.
- [ ] **Step 5: Commit** `static_nodes: apply static-nodes-permissions.conf`.

### Task 3: wire into PID 1 + docs

**Files:** Modify `init.c` (include + first-boot block ~line 1712), `static_nodes.h` (driver), `docs/how-it-works.md` or `docs/components.md` (whichever lists boot steps — one line), `docs/roadmap.md` if it tracks systemd parity.

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces: `void static_nodes_boot(void)` — `uname(2)` release; devname file `/lib/modules/<rel>/modules.devname` else `/usr/lib/modules/<rel>/modules.devname`; `sn_create_nodes("", …)`; summary log line (only when N>0); `sn_find_perm_file("")` → `sn_apply_perms`.

- [ ] **Step 1:** add `static_nodes_boot()` to the header; in `init.c` include `static_nodes.h` and call it right after `mount_pseudo();` (before `cleanup_tmp_locks()`), guarded `if (getpid() == 1)` like its neighbours.
- [ ] **Step 2:** `make` + `make test` (includes `check_tu_statics.sh init.o engine.o`) → all PASS.
- [ ] **Step 3: vmtest** via the schema-vmtest skill: boot the new binary as PID 1. Verify in the guest: boot log has `static nodes: created`; `ls -l /dev/net/tun` → `crw-rw-rw-` and `lsmod | grep -w tun` empty; then `exec 3<>/dev/net/tun` (or `ip tuntap add mode tun t0`) → `tun` now loaded; `/dev/uinput` exists; `/dev/kvm` group `kvm` if present.
- [ ] **Step 4:** docs line; commit `init: create static device nodes at boot (kmod-static-nodes parity)`.
- [ ] **Step 5:** push branch `feat/static-dev-nodes`, open **draft** PR; one `/code-review`; fold real findings.

### Task 4: hardware proof (after merge + COPR dev build)

- [ ] **Step 1:** DBox: install the dev build (`dnf upgrade --enablerepo` dev or the COPR rpm directly), confirm `rpm -q schema-init` shows the merge commit.
- [ ] **Step 2:** `schema-ctl reboot`; no manual modprobe; nobody logs in. Over ssh: `/dev/net/tun` 0666, `podman ps` shows `systemd-e2e-web` Up, `rail.log` network-online spawn→done ≈ 0 s, curl from blakbox `:8080` (after runtime `firewall-cmd --add-port=8080/tcp`) → 200. That is podman test 1 complete.
- [ ] **Step 3:** blakbox/Eli/Optiplex on next upgrade: `/dev/uinput` and `/dev/vhost-net` present.
- [ ] **Step 4:** clean up DBox: remove `e2e-web.container`, `podman rmi nginx:alpine`, `loginctl disable-linger ajax80`.
