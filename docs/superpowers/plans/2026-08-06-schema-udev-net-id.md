# schema-udev net_id builtin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce udev's `net_id` builtin — synthesize `ID_NET_NAMING_SCHEME` / `ID_NET_NAME_MAC` / `ID_NET_NAME_PATH` / `ID_NET_NAME_SLOT` / `ID_NET_NAME_ONBOARD` / `ID_NET_LABEL_ONBOARD` from a network interface's sysfs — byte-for-byte across the 9 net devices on blakbox, with the PCI + USB + platform + devicetree bus branches implemented.

**Architecture:** New header-only `net_id.h`, a faithful port of systemd v259 `src/udev/udev-builtin-net_id.c`. It reads the interface node's sysfs (`ifindex`/`iflink`/`type`/`addr_*`/`address`/`uevent`), gates on stacked + ARPHRD, stamps the naming scheme, derives the prefix, builds the MAC name, walks to the bus parent, and dispatches to the bus-specific namer. Mechanism only — wired to nothing. `schema-udev.c`/`.h` stay byte-identical.

**Tech Stack:** C99, `-O2 -Wall -Wextra -D_GNU_SOURCE`, GNU Make. Reuses `path_id.h` (`pi_sysattr`, `pi_parent`, `pi_base`, `pi_subsystem`, `safe_copy`) and `schema-udev.h` (`struct uevent`, `uevent_get`, `UE_MAX_KEYS`, `UE_KEY_MAX`, `UE_VAL_MAX`).

## Global Constraints

- **Boundary:** `schema-udev.c` and `schema-udev.h` MUST remain byte-identical to master. `grep net_id schema-udev.c` MUST be empty. The builtin is off by default, wired to nothing.
- **Normative source:** systemd v259 `src/udev/udev-builtin-net_id.c`. Where this plan and that source disagree, **the source governs and the live parity gate is the authority.** The USB/platform/devicetree branches are NOT exercised by the blakbox live gate (no such hardware here) — for those, port the exact byte format from the source and let the unit tests lock it; the format strings quoted in this plan are taken verbatim from v259.
- **Emit values verbatim, never `=0`-style flags.** `ID_NET_*` values are real strings (`v259`, `enxa8a1590be8ef`, `enp6s0`). Absence == not applicable. Emit a key at most once.
- **net_id emits ONLY these six keys:** `ID_NET_NAMING_SCHEME`, `ID_NET_NAME_MAC`, `ID_NET_NAME_ONBOARD`, `ID_NET_LABEL_ONBOARD`, `ID_NET_NAME_PATH`, `ID_NET_NAME_SLOT`. It does **NOT** emit `ID_NET_DRIVER`, `ID_NET_NAME`, `ID_PATH`, `ID_BUS`, `ID_VENDOR_ID`, `ID_MODEL_ID` (owned by net_setup_link / path_id / rules). No ethtool, no ioctl, no socket — pure sysfs file reads.
- **Naming scheme is pinned `v259`** — the version this box's udev stamped. It is the one version-coupled constant; a future scheme bump is a one-line `#define` change plus a live-gate re-baseline.
- **Gate order is load-bearing:** stacked (`iflink != ifindex`) → bail emitting nothing; ARPHRD not in {1 ether, 256 slip, 32 infiniband} → bail emitting nothing; only then stamp scheme + name.
- **`net_id.h` includes `path_id.h`** (transitively `schema-udev.h`) — do not re-include or re-implement its primitives (DRY).
- Reuse the `nid_emit(out, k, v)` helper for every property; guard on `out->n < UE_MAX_KEYS`.
- Full-line exact match is the parity standard, **both directions** (wrong value AND under/over-emission).

---

### Task 1: `net_id.h` scaffold — gates, scheme stamp, prefix

**Files:**
- Create: `net_id.h`
- Test: `tests/test_net_id.c`
- Modify: `Makefile` (add the test build line)

**Interfaces:**
- Consumes: `path_id.h` (`pi_sysattr`, `pi_parent`, `pi_base`, `pi_subsystem`, `safe_copy`), `schema-udev.h` (`struct uevent`, `UE_MAX_KEYS`, `UE_KEY_MAX`, `UE_VAL_MAX`, `uevent_get`).
- Produces:
  - `#define NID_ARPHRD_ETHER 1`, `NID_ARPHRD_INFINIBAND 32`, `NID_ARPHRD_SLIP 256`, `NID_NAMING_SCHEME "v259"`, `NID_ONBOARD_INDEX_MAX ((1U << 14) - 1)`.
  - `void nid_emit(struct uevent *out, const char *k, const char *v)`.
  - `int nid_uevent_val(const char *devdir, const char *key, char *out, size_t outsz)` — read `key=` from `<devdir>/uevent`; 0/-1.
  - `int nid_is_stacked(const char *netdir)` — 1 if `iflink != ifindex`.
  - `int nid_arphrd(const char *netdir)` — the `type` int, or -1.
  - `int nid_prefix(const char *netdir, int arphrd, char *out, size_t outsz)` — `en`/`wl`/`ww`/`sl`/`ib`; -1 if unsupported.

- [ ] **Step 1: Write the failing test**

Create `tests/test_net_id.c`:

```c
#include "net_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

static void nid_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static void nid_wf(const char *dir, const char *name, const char *val) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(val, f); fputc('\n', f); fclose(f);
}

static void test_gates(void) {
    char root[] = "/tmp/nidgateXXXXXX"; assert(mkdtemp(root));
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/eth0", root); nid_mkdirs(net);

    /* not stacked: ifindex == iflink */
    nid_wf(net, "ifindex", "3"); nid_wf(net, "iflink", "3");
    assert(nid_is_stacked(net) == 0);
    /* stacked: differ */
    nid_wf(net, "iflink", "2");
    assert(nid_is_stacked(net) == 1);

    /* arphrd */
    nid_wf(net, "type", "1");
    assert(nid_arphrd(net) == 1);

    /* prefix: plain ether -> en (uevent without DEVTYPE) */
    nid_wf(net, "uevent", "INTERFACE=eth0\nIFINDEX=3");
    char pfx[8];
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "en") == 0);

    /* wlan -> wl */
    nid_wf(net, "uevent", "INTERFACE=wlan0\nDEVTYPE=wlan");
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "wl") == 0);

    /* wwan -> ww */
    nid_wf(net, "uevent", "DEVTYPE=wwan");
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "ww") == 0);

    /* slip -> sl, infiniband -> ib */
    assert(nid_prefix(net, NID_ARPHRD_SLIP, pfx, sizeof pfx) == 0 && strcmp(pfx, "sl") == 0);
    assert(nid_prefix(net, NID_ARPHRD_INFINIBAND, pfx, sizeof pfx) == 0 && strcmp(pfx, "ib") == 0);

    /* unsupported arphrd -> -1 */
    assert(nid_prefix(net, 772, pfx, sizeof pfx) != 0);   /* loopback */

    printf("test_gates OK\n");
}

int main(void) {
    test_gates();
    printf("ALL net_id tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `net_id.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `net_id.h`:

```c
#ifndef SCHEMA_NET_ID_H
#define SCHEMA_NET_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define NID_ARPHRD_ETHER       1
#define NID_ARPHRD_INFINIBAND  32
#define NID_ARPHRD_SLIP        256
#define NID_NAMING_SCHEME      "v259"
#define NID_ONBOARD_INDEX_MAX  ((1U << 14) - 1)

static inline void nid_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

static inline int nid_uevent_val(const char *devdir, const char *key, char *out, size_t outsz) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/uevent", devdir) >= sizeof p) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t klen = strlen(key);
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            safe_copy(out, line + klen + 1, outsz);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static inline int nid_is_stacked(const char *netdir) {
    char a[64], b[64];
    if (pi_sysattr(netdir, "ifindex", a, sizeof a) != 0) return 0;
    if (pi_sysattr(netdir, "iflink",  b, sizeof b) != 0) return 0;
    return strcmp(a, b) != 0;
}

static inline int nid_arphrd(const char *netdir) {
    char t[64];
    if (pi_sysattr(netdir, "type", t, sizeof t) != 0) return -1;
    return atoi(t);
}

static inline int nid_prefix(const char *netdir, int arphrd, char *out, size_t outsz) {
    if (arphrd == NID_ARPHRD_INFINIBAND) { safe_copy(out, "ib", outsz); return 0; }
    if (arphrd == NID_ARPHRD_SLIP)       { safe_copy(out, "sl", outsz); return 0; }
    if (arphrd == NID_ARPHRD_ETHER) {
        char dt[64];
        if (nid_uevent_val(netdir, "DEVTYPE", dt, sizeof dt) == 0) {
            if (strcmp(dt, "wlan") == 0) { safe_copy(out, "wl", outsz); return 0; }
            if (strcmp(dt, "wwan") == 0) { safe_copy(out, "ww", outsz); return 0; }
        }
        safe_copy(out, "en", outsz);
        return 0;
    }
    return -1;
}

#endif /* SCHEMA_NET_ID_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_gates OK` / `ALL net_id tests passed`. No warnings.

- [ ] **Step 5: Add the Makefile test line**

In `Makefile`, in the `test:` target, after the `test_input_id.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_net_id.c -o /tmp/schema-test-netid && /tmp/schema-test-netid
```

- [ ] **Step 6: Commit**

```bash
git add net_id.h tests/test_net_id.c Makefile
git commit -m "feat(net_id): scaffold builtin header — gates, scheme, prefix"
```

---

### Task 2: MAC name

**Files:**
- Modify: `net_id.h`
- Test: `tests/test_net_id.c`

**Interfaces:**
- Consumes: `pi_sysattr`, `safe_copy`.
- Produces: `int nid_mac_name(const char *netdir, const char *prefix, int arphrd, char *out, size_t outsz)` — writes `<prefix>x<12 lowercase hex>` and returns 0 when `addr_assign_type==0` (NET_ADDR_PERM) AND `addr_len==6` AND arphrd != infiniband; else -1 (no emission).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net_id.c`, and call `test_mac();` from `main` before the final print:

```c
static void test_mac(void) {
    char root[] = "/tmp/nidmacXXXXXX"; assert(mkdtemp(root));
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/eth0", root); nid_mkdirs(net);
    char name[64];

    /* permanent, 6-byte -> enx<hex, colons stripped> */
    nid_wf(net, "addr_assign_type", "0");
    nid_wf(net, "addr_len", "6");
    nid_wf(net, "address", "a8:a1:59:0b:e8:ef");
    assert(nid_mac_name(net, "en", NID_ARPHRD_ETHER, name, sizeof name) == 0);
    assert(strcmp(name, "enxa8a1590be8ef") == 0);

    /* random assign type (1) -> no name */
    nid_wf(net, "addr_assign_type", "1");
    assert(nid_mac_name(net, "en", NID_ARPHRD_ETHER, name, sizeof name) != 0);

    /* permanent but not 6 bytes -> no name */
    nid_wf(net, "addr_assign_type", "0");
    nid_wf(net, "addr_len", "20");
    assert(nid_mac_name(net, "ib", NID_ARPHRD_INFINIBAND, name, sizeof name) != 0);

    printf("test_mac OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `nid_mac_name` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `net_id.h` before the `#endif`:

```c
static inline int nid_mac_name(const char *netdir, const char *prefix, int arphrd,
                               char *out, size_t outsz) {
    if (arphrd == NID_ARPHRD_INFINIBAND) return -1;
    char aat[64], alen[64], addr[64];
    if (pi_sysattr(netdir, "addr_assign_type", aat, sizeof aat) != 0) return -1;
    if (atoi(aat) != 0) return -1;                 /* not NET_ADDR_PERM */
    if (pi_sysattr(netdir, "addr_len", alen, sizeof alen) != 0) return -1;
    if (atoi(alen) != 6) return -1;
    if (pi_sysattr(netdir, "address", addr, sizeof addr) != 0) return -1;

    char hex[32]; size_t j = 0;
    for (size_t i = 0; addr[i] && j + 1 < sizeof hex; i++)
        if (addr[i] != ':') hex[j++] = addr[i];
    hex[j] = '\0';
    if (j != 12) return -1;

    snprintf(out, outsz, "%sx%s", prefix, hex);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_mac OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add net_id.h tests/test_net_id.c
git commit -m "feat(net_id): permanent-MAC name (enx<hex>)"
```

---

### Task 3: bus-parent resolution + PCI names (the live-anchored branch)

**Files:**
- Modify: `net_id.h`
- Test: `tests/test_net_id.c`

**Interfaces:**
- Consumes: `pi_parent`, `pi_subsystem`, `pi_base`, `pi_sysattr`, `nid_emit`.
- Produces:
  - `int nid_find_bus_parent(const char *sysroot, const char *devpath, char *busdir, size_t bussz, char *sub, size_t subsz)` — from `sysroot+devpath`, climb from the net device's parent to the first ancestor whose subsystem is `pci`/`usb`/`platform`/`of`; writes its dir + subsystem name; 0/-1.
  - `int nid_pci_multifunction(const char *pcidir)` — 1 if PCI config header type (byte 0x0e) has bit 7 set.
  - `void nid_names_pci(const char *sysroot, const char *pcidir, const char *prefix, struct uevent *out)` — emits `ID_NET_NAME_PATH`, and (when present) `ID_NET_NAME_SLOT`, `ID_NET_NAME_ONBOARD`, `ID_NET_LABEL_ONBOARD`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net_id.c`; helper `nid_has` mirrors the sibling builtins. Call `test_pci();` from `main`:

```c
static int nid_has_val(const struct uevent *ev, const char *k, const char *v) {
    const char *g = uevent_get(ev, k);
    return g && strcmp(g, v) == 0;
}
static int nid_absent(const struct uevent *ev, const char *k) {
    return uevent_get(ev, k) == NULL;
}

static void test_pci(void) {
    char root[] = "/tmp/nidpciXXXXXX"; assert(mkdtemp(root));

    /* /sys/devices/pci0000:00/0000:00:1c.0/0000:06:00.0/net/enp6s0 */
    char pci[PATH_MAX];
    snprintf(pci, sizeof pci, "%s/devices/pci0000:00/0000:00:1c.0/0000:06:00.0", root);
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/enp6s0", pci); nid_mkdirs(net);
    nid_wf(pci, "dev_port", "0");
    /* config: 64 bytes, header type (offset 0x0e) = 0x00 -> single function */
    { char cf[PATH_MAX]; snprintf(cf, sizeof cf, "%s/config", pci);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z, 1, 64, f); fclose(f); }
    /* subsystem symlinks so pi_subsystem resolves */
    { char link[PATH_MAX], tgt[PATH_MAX];
      snprintf(tgt, sizeof tgt, "%s/bus/pci", root); nid_mkdirs(tgt);
      snprintf(link, sizeof link, "%s/subsystem", pci); symlink(tgt, link); }

    char busdir[PATH_MAX], sub[64];
    assert(nid_find_bus_parent(root, "/devices/pci0000:00/0000:00:1c.0/0000:06:00.0/net/enp6s0",
                               busdir, sizeof busdir, sub, sizeof sub) == 0);
    assert(strcmp(sub, "pci") == 0);

    struct uevent e; e.n = 0;
    nid_names_pci(root, busdir, "en", &e);
    assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enp6s0"));   /* bus 6, slot 0, func 0 single-fn */
    assert(nid_absent(&e, "ID_NET_NAME_SLOT"));              /* no hotplug slot */
    assert(nid_absent(&e, "ID_NET_NAME_ONBOARD"));

    /* nonzero domain + func>0 + dev_port>0: 0001:1a:00.1, dev_port 2 -> enP1p26s0f1d2 */
    char pci2[PATH_MAX];
    snprintf(pci2, sizeof pci2, "%s/devices/pci0001:1a/0001:1a:00.1", root);
    char net2[PATH_MAX]; snprintf(net2, sizeof net2, "%s/net/x", pci2); nid_mkdirs(net2);
    nid_wf(pci2, "dev_port", "2");
    { char cf[PATH_MAX]; snprintf(cf, sizeof cf, "%s/config", pci2);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z, 1, 64, f); fclose(f); }
    struct uevent e2; e2.n = 0;
    nid_names_pci(root, pci2, "en", &e2);
    assert(nid_has_val(&e2, "ID_NET_NAME_PATH", "enP1p26s0f1d2"));

    /* onboard: acpi_index 3 + label -> ID_NET_NAME_ONBOARD=eno3, ID_NET_LABEL_ONBOARD verbatim */
    nid_wf(pci, "acpi_index", "3");
    nid_wf(pci, "label", "Onboard LAN");
    struct uevent e3; e3.n = 0;
    nid_names_pci(root, pci, "en", &e3);
    assert(nid_has_val(&e3, "ID_NET_NAME_ONBOARD", "eno3"));
    assert(nid_has_val(&e3, "ID_NET_LABEL_ONBOARD", "Onboard LAN"));

    /* hotplug slot: /sys/bus/pci/slots/5/address = 0000:06:00 -> ID_NET_NAME_SLOT=ens5 */
    { char sd[PATH_MAX]; snprintf(sd, sizeof sd, "%s/bus/pci/slots/5", root); nid_mkdirs(sd);
      nid_wf(sd, "address", "0000:06:00"); }
    struct uevent e4; e4.n = 0;
    nid_names_pci(root, pci, "en", &e4);
    assert(nid_has_val(&e4, "ID_NET_NAME_SLOT", "ens5"));

    printf("test_pci OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `nid_find_bus_parent` / `nid_names_pci` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `net_id.h` before the `#endif`:

```c
static inline int nid_find_bus_parent(const char *sysroot, const char *devpath,
                                      char *busdir, size_t bussz, char *sub, size_t subsz) {
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur) return -1;
    if (pi_parent(cur) != 0) return -1;   /* start at the net device's parent */
    for (;;) {
        char s[128];
        if (pi_subsystem(cur, s, sizeof s) == 0 &&
            (strcmp(s, "pci") == 0 || strcmp(s, "usb") == 0 ||
             strcmp(s, "platform") == 0 || strcmp(s, "of") == 0)) {
            safe_copy(busdir, cur, bussz);
            safe_copy(sub, s, subsz);
            return 0;
        }
        if (pi_parent(cur) != 0) return -1;
    }
}

static inline int nid_pci_multifunction(const char *pcidir) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s/config", pcidir) >= sizeof p) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int multi = 0;
    if (fseek(f, 0x0e, SEEK_SET) == 0) {
        int c = fgetc(f);
        if (c != EOF && (c & 0x80)) multi = 1;
    }
    fclose(f);
    return multi;
}

/* Find a PCI hotplug slot number whose slots/<N>/address is a prefix of dom:bus:slot.
 * Writes the slot dir name to out; 0/-1. */
static inline int nid_pci_slot(const char *sysroot, unsigned dom, unsigned bus, unsigned slot,
                               char *out, size_t outsz) {
    char slotsdir[PATH_MAX];
    if ((size_t)snprintf(slotsdir, sizeof slotsdir, "%s/bus/pci/slots", sysroot) >= sizeof slotsdir)
        return -1;
    DIR *d = opendir(slotsdir);
    if (!d) return -1;
    char want[32];
    snprintf(want, sizeof want, "%04x:%02x:%02x", dom, bus, slot);
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char ad[PATH_MAX], val[64];
        snprintf(ad, sizeof ad, "%s/%s", slotsdir, e->d_name);
        if (pi_sysattr(ad, "address", val, sizeof val) != 0) continue;
        if (strncmp(val, want, strlen(want)) == 0) { safe_copy(out, e->d_name, outsz); found = 0; break; }
    }
    closedir(d);
    return found;
}

static inline void nid_names_pci(const char *sysroot, const char *pcidir, const char *prefix,
                                 struct uevent *out) {
    unsigned dom, bus, slot, func;
    if (sscanf(pi_base(pcidir), "%x:%x:%x.%x", &dom, &bus, &slot, &func) != 4) return;

    char dp[64]; int dev_port = 0;
    if (pi_sysattr(pcidir, "dev_port", dp, sizeof dp) == 0) dev_port = atoi(dp);
    int multi = nid_pci_multifunction(pcidir);

    /* shared func/dev_port suffix */
    char suffix[32]; size_t so = 0;
    if (func > 0 || multi) so += (size_t)snprintf(suffix + so, sizeof suffix - so, "f%u", func);
    if (dev_port > 0)      so += (size_t)snprintf(suffix + so, sizeof suffix - so, "d%u", dev_port);
    (void)so;

    char domstr[16]; domstr[0] = '\0';
    if (dom > 0) snprintf(domstr, sizeof domstr, "P%u", dom);

    /* ID_NET_NAME_PATH */
    char path[128];
    snprintf(path, sizeof path, "%s%sp%us%u%s", prefix, domstr, bus, slot, suffix);
    nid_emit(out, "ID_NET_NAME_PATH", path);

    /* ID_NET_NAME_SLOT (hotplug) */
    char slotname[64];
    if (nid_pci_slot(sysroot, dom, bus, slot, slotname, sizeof slotname) == 0) {
        char sl[128];
        snprintf(sl, sizeof sl, "%s%ss%s%s", prefix, domstr, slotname, suffix);
        nid_emit(out, "ID_NET_NAME_SLOT", sl);
    }

    /* ID_NET_NAME_ONBOARD (acpi_index preferred, else index) */
    char idxbuf[64]; int idx = 0;
    if (pi_sysattr(pcidir, "acpi_index", idxbuf, sizeof idxbuf) == 0) idx = atoi(idxbuf);
    else if (pi_sysattr(pcidir, "index", idxbuf, sizeof idxbuf) == 0) idx = atoi(idxbuf);
    if (idx > 0 && (unsigned)idx <= NID_ONBOARD_INDEX_MAX) {
        char ob[128];
        snprintf(ob, sizeof ob, "%so%d", prefix, idx);
        nid_emit(out, "ID_NET_NAME_ONBOARD", ob);
    }

    /* ID_NET_LABEL_ONBOARD (firmware label, verbatim) */
    char label[128];
    if (pi_sysattr(pcidir, "label", label, sizeof label) == 0 && label[0])
        nid_emit(out, "ID_NET_LABEL_ONBOARD", label);
}
```

Note (source-parity): systemd's `names_pci_onboard` may append a `phys_port_name` (`n<name>`) or `dev_port` (`d<n>`) suffix to ONBOARD, and its slot name can carry the same func/port suffix. blakbox exercises none of these; the format strings above (`%so%u`, `%s%ss%u`, `%sa...`) are the v259 verbatim skeletons — reconcile any residual detail toward the source.

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_pci OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add net_id.h tests/test_net_id.c
git commit -m "feat(net_id): bus-parent walk + PCI name/slot/onboard/label"
```

---

### Task 4: USB, platform, devicetree branches

**Files:**
- Modify: `net_id.h`
- Test: `tests/test_net_id.c`

**Interfaces:**
- Consumes: `pi_base`, `pi_sysattr`, `pi_parent`, `pi_subsystem`, `nid_names_pci`, `nid_emit`.
- Produces:
  - `int nid_usb_specifier(const char *usbdir, char *out, size_t outsz)` — parses the USB sysname `<bus>-<ports>:<config>.<iface>`; writes `u<ports with '.'→'u'>[c<config>][i<iface>]`; 0/-1.
  - `void nid_names_usb(const char *sysroot, const char *usbdir, const char *prefix, struct uevent *out)` — appends the USB specifier to the PCI-parent path (delegating to `nid_names_pci` with the specifier folded in), or emits `<prefix><specifier>` when there is no PCI parent.
  - `void nid_names_platform(const char *platdir, const char *prefix, struct uevent *out)` — ACPI id → `ID_NET_NAME_PATH = <prefix>a<vendor><hexmodel>i<instance>`.
  - `void nid_names_devicetree(const char *sysroot, const char *netdir, const char *prefix, struct uevent *out)` — DT alias index → `ID_NET_NAME_PATH = <prefix>d<N>`.

**These branches are not on blakbox — port the exact bytes from systemd v259; the unit tests below lock the verbatim format strings.**

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net_id.c`. Call `test_other_buses();` from `main`:

```c
static void test_other_buses(void) {
    char root[] = "/tmp/nidbusXXXXXX"; assert(mkdtemp(root));

    /* USB specifier: "1-1.2:1.0" -> ports "1.2"->"1u2", config 1 (omitted), iface 0 (omitted) */
    char spec[64];
    { char usb[PATH_MAX]; snprintf(usb, sizeof usb, "%s/1-1.2:1.0", root); nid_mkdirs(usb);
      assert(nid_usb_specifier(usb, spec, sizeof spec) == 0);
      assert(strcmp(spec, "u1u2") == 0); }

    /* USB specifier with config 2 + iface 3: "2-1:2.3" -> "u1c2i3" */
    { char usb[PATH_MAX]; snprintf(usb, sizeof usb, "%s/2-1:2.3", root); nid_mkdirs(usb);
      assert(nid_usb_specifier(usb, spec, sizeof spec) == 0);
      assert(strcmp(spec, "u1c2i3") == 0); }

    /* platform: ACPI id "ETH0000:02" (vendor eth, model 0x0000, instance 2) -> enaeth0i2 */
    { char plat[PATH_MAX]; snprintf(plat, sizeof plat, "%s/devices/platform/ETH0000:02", root);
      nid_mkdirs(plat);
      struct uevent e; e.n = 0;
      nid_names_platform(plat, "en", &e);
      assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enaeth0i2")); }

    /* malformed platform id (colon misplaced) -> nothing */
    { char plat[PATH_MAX]; snprintf(plat, sizeof plat, "%s/devices/platform/BADID00000", root);
      nid_mkdirs(plat);
      struct uevent e; e.n = 0;
      nid_names_platform(plat, "en", &e);
      assert(e.n == 0); }

    /* devicetree output format literal (DT alias layout can't be faithfully faked in a tmpdir;
       nid_names_devicetree is ported from source and asserted live on the Pis, not here) */
    { char name[32]; snprintf(name, sizeof name, "%sd%u", "en", 0u);
      assert(strcmp(name, "end0") == 0); }

    printf("test_other_buses OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `nid_usb_specifier` / `nid_names_platform` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `net_id.h` before the `#endif`. Port each from systemd v259; these are the verbatim format strings:

```c
/* USB sysname: "<bus>-<ports>:<config>.<iface>"; ports '.'→'u'; drop config==1, iface==0 */
static inline int nid_usb_specifier(const char *usbdir, char *out, size_t outsz) {
    const char *name = pi_base(usbdir);
    const char *dash = strchr(name, '-');
    if (!dash) return -1;
    char ports[64] = "", cfg[16] = "", iface[16] = "";
    const char *colon = strchr(dash, ':');
    if (colon) {
        size_t plen = (size_t)(colon - (dash + 1));
        if (plen >= sizeof ports) return -1;
        memcpy(ports, dash + 1, plen); ports[plen] = '\0';
        const char *dot = strchr(colon, '.');
        if (dot) {
            char cbuf[16];
            size_t clen = (size_t)(dot - (colon + 1));
            if (clen >= sizeof cbuf) return -1;
            memcpy(cbuf, colon + 1, clen); cbuf[clen] = '\0';
            if (atoi(cbuf) != 1) snprintf(cfg, sizeof cfg, "c%d", atoi(cbuf));
            if (atoi(dot + 1) != 0) snprintf(iface, sizeof iface, "i%d", atoi(dot + 1));
        }
    } else {
        safe_copy(ports, dash + 1, sizeof ports);
    }
    for (char *s = ports; *s; s++) if (*s == '.') *s = 'u';   /* "1.2" -> "1u2" */
    snprintf(out, outsz, "u%s%s%s", ports, cfg, iface);
    return 0;
}

static inline void nid_names_usb(const char *sysroot, const char *usbdir, const char *prefix,
                                 struct uevent *out) {
    char spec[64];
    if (nid_usb_specifier(usbdir, spec, sizeof spec) != 0) return;

    /* climb to a PCI parent; if found, name is the PCI path with the USB specifier appended */
    char cur[PATH_MAX]; safe_copy(cur, usbdir, sizeof cur);
    for (;;) {
        if (pi_parent(cur) != 0) { cur[0] = '\0'; break; }
        char s[128];
        if (pi_subsystem(cur, s, sizeof s) == 0 && strcmp(s, "pci") == 0) break;
    }
    if (cur[0]) {
        /* delegate: build the PCI path, then fold in the USB specifier */
        struct uevent tmp; tmp.n = 0;
        nid_names_pci(sysroot, cur, prefix, &tmp);
        const char *pcipath = uevent_get(&tmp, "ID_NET_NAME_PATH");
        if (pcipath) {
            char full[192];
            snprintf(full, sizeof full, "%s%s", pcipath, spec);
            nid_emit(out, "ID_NET_NAME_PATH", full);
        }
    } else {
        char full[128];
        snprintf(full, sizeof full, "%s%s", prefix, spec);
        nid_emit(out, "ID_NET_NAME_PATH", full);
    }
}

/* ACPI platform id "<vendor><model>:<instance>": 3- or 4-char alpha vendor,
 * 4 hex model digits, colon at index 7 (len 10) or 8 (len 11), then decimal instance.
 * ID_NET_NAME_PATH = <prefix>a<vendor lowercase><hex model>i<instance>
 * e.g. "ETH0000:02" -> "enaeth0i2" */
static inline void nid_names_platform(const char *platdir, const char *prefix, struct uevent *out) {
    const char *id = pi_base(platdir);
    size_t len = strlen(id);
    if (len != 10 && len != 11) return;
    size_t vlen = (len == 10) ? 3 : 4;   /* vendor length */
    if (id[vlen + 4] != ':') return;     /* colon after vendor + 4 model digits */
    char vendor[8];
    for (size_t i = 0; i < vlen; i++) {
        char c = id[i];
        if (!(c >= 'A' && c <= 'Z')) return;   /* vendor is alpha */
        vendor[i] = (char)(c - 'A' + 'a');
    }
    vendor[vlen] = '\0';
    char modbuf[8];
    memcpy(modbuf, id + vlen, 4); modbuf[4] = '\0';
    unsigned model = (unsigned)strtoul(modbuf, NULL, 16);
    unsigned inst = (unsigned)strtoul(id + vlen + 5, NULL, 10);
    char path[64];
    snprintf(path, sizeof path, "%sa%s%xi%u", prefix, vendor, model, inst);
    nid_emit(out, "ID_NET_NAME_PATH", path);
}

/* DeviceTree alias: match the netdev's of_node against /firmware/devicetree aliases;
 * emit <prefix>d<index>. Port the alias-scan from systemd v259 names_devicetree. */
static inline void nid_names_devicetree(const char *sysroot, const char *netdir,
                                        const char *prefix, struct uevent *out) {
    /* Resolve <netdir>/of_node; find the alias whose target matches; index N -> "<prefix>dN".
     * The scan mirrors systemd names_devicetree: read /proc/device-tree/aliases entries,
     * resolve each to a node, compare against the device's of_node real path. */
    (void)sysroot; (void)netdir;
    char idx[16];
    if (pi_sysattr(netdir, "of_node/alias_index", idx, sizeof idx) != 0) return;  /* see note */
    char path[32];
    snprintf(path, sizeof path, "%sd%d", prefix, atoi(idx));
    nid_emit(out, "ID_NET_NAME_PATH", path);
}
```

Note on `nid_names_devicetree`: there is no `of_node/alias_index` sysattr in real sysfs — the alias index must be computed by scanning `/proc/device-tree/aliases` and matching the device's `of_node` real path, exactly as systemd `names_devicetree` does. Implement that scan from the source; the `%sd%u` format is the fixed output. The unit test asserts the format literal only (DT layout can't be faithfully faked in a tmpdir).

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_other_buses OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add net_id.h tests/test_net_id.c
git commit -m "feat(net_id): USB / platform / devicetree name branches"
```

---

### Task 5: orchestrator `net_id_build`

**Files:**
- Modify: `net_id.h`
- Test: `tests/test_net_id.c`

**Interfaces:**
- Consumes: `nid_is_stacked`, `nid_arphrd`, `nid_prefix`, `nid_mac_name`, `nid_find_bus_parent`, `nid_names_pci`, `nid_names_usb`, `nid_names_platform`, `nid_names_devicetree`, `nid_emit`.
- Produces: `int net_id_build(const char *sysroot, const char *devpath, struct uevent *out)` — gate → scheme → mac → devicetree → bus dispatch; returns 0 always (0 emitted keys is a valid result for stacked/unsupported), fills `out`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net_id.c`. Call `test_build();` from `main`:

```c
static void test_build(void) {
    char root[] = "/tmp/nidbuildXXXXXX"; assert(mkdtemp(root));

    /* full PCI ethernet, permanent MAC */
    char pci[PATH_MAX];
    snprintf(pci, sizeof pci, "%s/devices/pci0000:00/0000:06:00.0", root);
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/enp6s0", pci); nid_mkdirs(net);
    nid_wf(net, "ifindex", "2"); nid_wf(net, "iflink", "2");
    nid_wf(net, "type", "1");
    nid_wf(net, "addr_assign_type", "0"); nid_wf(net, "addr_len", "6");
    nid_wf(net, "address", "a8:a1:59:0b:e8:ef");
    nid_wf(net, "uevent", "DEVTYPE=");
    nid_wf(pci, "dev_port", "0");
    { char cf[PATH_MAX]; snprintf(cf, sizeof cf, "%s/config", pci);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z,1,64,f); fclose(f); }
    { char tgt[PATH_MAX]; snprintf(tgt, sizeof tgt, "%s/bus/pci", root); nid_mkdirs(tgt);
      char link[PATH_MAX]; snprintf(link, sizeof link, "%s/subsystem", pci); symlink(tgt, link); }

    struct uevent e;
    assert(net_id_build(root, "/devices/pci0000:00/0000:06:00.0/net/enp6s0", &e) == 0);
    assert(nid_has_val(&e, "ID_NET_NAMING_SCHEME", "v259"));
    assert(nid_has_val(&e, "ID_NET_NAME_MAC", "enxa8a1590be8ef"));
    assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enp6s0"));

    /* stacked: iflink != ifindex -> nothing */
    char v[PATH_MAX]; snprintf(v, sizeof v, "%s/devices/virt/net/veth0", root); nid_mkdirs(v);
    nid_wf(v, "ifindex", "9"); nid_wf(v, "iflink", "8"); nid_wf(v, "type", "1");
    assert(net_id_build(root, "/devices/virt/net/veth0", &e) == 0);
    assert(e.n == 0);

    /* unsupported ARPHRD (loopback 772) -> nothing */
    char lo[PATH_MAX]; snprintf(lo, sizeof lo, "%s/devices/virt/net/lo", root); nid_mkdirs(lo);
    nid_wf(lo, "ifindex", "1"); nid_wf(lo, "iflink", "1"); nid_wf(lo, "type", "772");
    assert(net_id_build(root, "/devices/virt/net/lo", &e) == 0);
    assert(e.n == 0);

    /* standalone virtual ether (docker0): not stacked, ether, no bus parent -> scheme only */
    char dk[PATH_MAX]; snprintf(dk, sizeof dk, "%s/devices/virt/net/docker0", root); nid_mkdirs(dk);
    nid_wf(dk, "ifindex", "4"); nid_wf(dk, "iflink", "4"); nid_wf(dk, "type", "1");
    nid_wf(dk, "addr_assign_type", "3"); nid_wf(dk, "addr_len", "6");
    nid_wf(dk, "address", "02:42:aa:bb:cc:dd"); nid_wf(dk, "uevent", "DEVTYPE=");
    assert(net_id_build(root, "/devices/virt/net/docker0", &e) == 0);
    assert(nid_has_val(&e, "ID_NET_NAMING_SCHEME", "v259"));
    assert(nid_absent(&e, "ID_NET_NAME_PATH"));
    assert(nid_absent(&e, "ID_NET_NAME_MAC"));   /* addr_assign_type 3, not permanent */

    printf("test_build OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_net_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `net_id_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `net_id.h` before the `#endif`:

```c
static inline int net_id_build(const char *sysroot, const char *devpath, struct uevent *out) {
    out->n = 0;
    char netdir[PATH_MAX];
    if ((size_t)snprintf(netdir, sizeof netdir, "%s%s", sysroot, devpath) >= sizeof netdir) return 0;

    if (nid_is_stacked(netdir)) return 0;

    int arphrd = nid_arphrd(netdir);
    if (arphrd != NID_ARPHRD_ETHER && arphrd != NID_ARPHRD_SLIP && arphrd != NID_ARPHRD_INFINIBAND)
        return 0;

    nid_emit(out, "ID_NET_NAMING_SCHEME", NID_NAMING_SCHEME);

    char prefix[8];
    if (nid_prefix(netdir, arphrd, prefix, sizeof prefix) != 0) return 0;

    char mac[64];
    if (nid_mac_name(netdir, prefix, arphrd, mac, sizeof mac) == 0)
        nid_emit(out, "ID_NET_NAME_MAC", mac);

    /* devicetree may provide NAME_PATH independent of the bus parent */
    nid_names_devicetree(sysroot, netdir, prefix, out);
    int have_path = uevent_get(out, "ID_NET_NAME_PATH") != NULL;

    char busdir[PATH_MAX], sub[64];
    if (nid_find_bus_parent(sysroot, devpath, busdir, sizeof busdir, sub, sizeof sub) == 0) {
        if (strcmp(sub, "pci") == 0)            nid_names_pci(sysroot, busdir, prefix, out);
        else if (strcmp(sub, "usb") == 0)       nid_names_usb(sysroot, busdir, prefix, out);
        else if (strcmp(sub, "platform") == 0 && !have_path)
                                                nid_names_platform(busdir, prefix, out);
    }
    return 0;
}
```

- [ ] **Step 4: Run the full suite**

Run: `make test`
Expected: PASS — every existing suite plus `test_gates` / `test_mac` / `test_pci` / `test_other_buses` / `test_build` / `ALL net_id tests passed`. No warnings.

- [ ] **Step 5: Verify the boundary**

Run: `git diff origin/master -- schema-udev.c schema-udev.h; grep -c net_id schema-udev.c`
Expected: empty diff, `0`.

- [ ] **Step 6: Commit**

```bash
git add net_id.h tests/test_net_id.c
git commit -m "feat(net_id): orchestrator net_id_build + dispatch"
```

---

### Task 6: live parity harness + vmtest

**Files:**
- Create: `tests/verify_net_id_live.sh`

**Interfaces:**
- Consumes: `net_id_build` from `net_id.h`.
- Produces: an executable acceptance script; prints `net_id live parity: N devices, M mismatches` and exits non-zero if M > 0.

- [ ] **Step 1: Write the harness**

Create `tests/verify_net_id_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run net_id_build over every /sys/class/net device, diff the
# net_id-owned keys vs `udevadm info` BOTH directions.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/netid_driver.c <<'EOF'
#include "net_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (net_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/netid_driver.c -o /tmp/netid_driver

# net_id owns exactly these keys; everything else in udev's set is out of scope.
KEYS='^(ID_NET_NAMING_SCHEME|ID_NET_NAME_MAC|ID_NET_NAME_PATH|ID_NET_NAME_SLOT|ID_NET_NAME_ONBOARD|ID_NET_LABEL_ONBOARD)='

props=$(mktemp)
netprops=$(mktemp)
misses=$(mktemp)
total=0
for ifp in /sys/class/net/*; do
    [ -e "$ifp" ] || continue
    dev=$(readlink -f "$ifp")             # /sys/devices/.../net/<if>
    devpath=${dev#/sys}
    total=$((total + 1))
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -E "$KEYS" "$props" > "$netprops" || true
    emitted=$(/tmp/netid_driver "$devpath")
    # forward: every key we emit must be present verbatim in udev's net_id subset
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$netprops" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    # reverse: every net_id key udev has, we must emit (under-emission)
    while IFS= read -r uline; do
        [ -n "$uline" ] || continue
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done < "$netprops" >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'net_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$netprops" "$misses"
[ "$miss" -eq 0 ]
```

- [ ] **Step 2: Run the live gate**

Run: `chmod +x tests/verify_net_id_live.sh && tests/verify_net_id_live.sh`
Expected: `net_id live parity: 9 devices, 0 mismatches` and exit 0.

If any `MISMATCH` prints: the emitted line and udev's value are shown. Fix the namer/parse in `net_id.h` (consult the systemd source), re-run. Do NOT touch `schema-udev.c`.

- [ ] **Step 3: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh` then `cd -`
Expected: `>> RESULT: PASS`. Confirms the new header does not disturb the PID-1 boot.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_net_id_live.sh
git commit -m "test(net_id): live udev parity acceptance harness (both directions)"
```

- [ ] **Step 5: Push and confirm origin == local (hard rule)**

```bash
git push -u origin feat/schema-udev-net-id
git ls-remote origin refs/heads/feat/schema-udev-net-id
git rev-parse HEAD
```

Expected: the two hashes are identical. Only then is the work landable.

---

## Notes for the implementer (Greg)

- **The live gate anchors PCI only.** enp6s0 → `ID_NET_NAMING_SCHEME=v259` + `ID_NET_NAME_PATH=enp6s0` + `ID_NET_NAME_MAC=enxa8a1590be8ef`; wlp5s0 → scheme + `ID_NET_NAME_PATH=wlp5s0` (no MAC, random addr); docker0/podman0 → scheme only; lo/wgnord/tailscale0/veth0/veth88d9c02 → nothing. Total 9 devices, 0 mismatches, both directions.
- **USB / platform / devicetree have no hardware here** — port their exact bytes from systemd v259 `udev-builtin-net_id.c`. The format strings in this plan (`u<ports '.'→'u'>`, `%sa%s%xi%u`, `%sd%u`, `%so%u`, `%s%ss%u`) are verbatim from v259; the unit tests lock them. Where a detail (USB config/iface handling, onboard `phys_port_name` suffix, DT alias scan) exceeds what the fabricated tests cover, the source governs.
- **Gate order is not cosmetic:** stacked check FIRST (veth pairs must emit nothing, not even the scheme), then ARPHRD, then scheme stamp. A device that fails either gate emits zero keys.
- `schema-udev.c` and `.h` are frozen. If a test seems to need a change there, stop — it doesn't.
- The live gate is the final authority. If the systemd source and this plan ever differ, follow the source and let the gate confirm.
- After I (Claire) verify all gates, the branch lands via PR. Do not open the PR yourself.
```
