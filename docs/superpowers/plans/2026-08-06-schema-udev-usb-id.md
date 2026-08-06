# schema-udev `usb_id` builtin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pure C builtin that reproduces systemd-udev's USB identification properties (`ID_*` / `ID_USB_*`) for a device, byte-for-byte, from USB device-descriptor sysattrs.

**Architecture:** A new header `usb_id.h` holds `usb_id_build(sysroot, devpath, struct uevent *out)`. It locates the device's `usb_device` (descriptor source) and `usb_interface` (interface-level props) in sysfs, applies two exact string encoders, composes the serial, maps the interface class to a type, enumerates interfaces, and fills a caller-owned `struct uevent`. Reads sysfs, writes nothing. NOT wired into the live daemon — mechanism only, same boundary as path_id (builtin #1).

**Tech Stack:** C (`-O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE`, static build), header-only inline in the style of `path_id.h` / `schema-udev.h`, `assert`-based unit tests that build fake sysfs trees under `mkdtemp` (the `tests/test_coldplug.c` / `tests/test_path_id.c` idiom).

## Global Constraints

- **Byte-parity is the acceptance bar.** Every format below is fixed by blakbox ground truth in `docs/superpowers/specs/2026-08-06-schema-udev-usb-id-design.md`. Do not invent or "clean up" formats.
- **`schema-udev.c` MUST stay byte-identical to master.** usb_id is never `#include`d or called from it. Boundary gate: `git diff master..HEAD -- schema-udev.c` empty AND `grep usb_id schema-udev.c` empty.
- **Read-only.** Never write sysfs, never open netlink, never touch `/run/udev`.
- **`usb_id.h` includes `path_id.h`** (landed on master) to reuse its sysfs primitives `pi_sysattr(devdir, attr, out, outsz)`, `pi_base(dir)`, `pi_parent(cur)`, `pi_subsystem(devdir, out, outsz)`. Do not duplicate them. `path_id.h` transitively provides `schema-udev.h` (`struct uevent`, `UE_KEY_MAX`, `UE_VAL_MAX`, `UE_MAX_KEYS`=32, `safe_copy`).
- **Output fits `UE_MAX_KEYS`=32** (≤24 keys emitted). No change to `schema-udev.h`.
- **Safe-char set (SAFE):** `A-Z a-z 0-9 # + - . : = @ _`. Verified: `-`, `.`, `:` are kept; space and `,` are not.
- **Plain form** (`ID_VENDOR`, `ID_MODEL`, serial parts): replace_whitespace (trim leading+trailing ws, collapse internal ws runs to a single `_`) THEN replace_chars (non-SAFE → `_`).
- **ENC form** (`ID_VENDOR_ENC`, `ID_MODEL_ENC`): per-char, SAFE verbatim else `\xNN` (lowercase hex); NO trim, NO collapse.
- **Fallbacks:** no `manufacturer` → `ID_VENDOR`/`_ENC` = raw `idVendor` hex; no `product` → `ID_MODEL`/`_ENC` = raw `idProduct` hex; no `serial` → omit `ID_SERIAL_SHORT` entirely.
- **`ID_SERIAL`** = `<ID_VENDOR>_<ID_MODEL>` + (`_<ID_SERIAL_SHORT>` if serial present).
- **`ID_USB_INTERFACES`** = `:`-wrapped `:`-delimited triplets (`bInterfaceClass`+`bInterfaceSubClass`+`bInterfaceProtocol`, each a 2-hex sysattr string concatenated), interfaces in ascending `bInterfaceNumber` order, duplicate triplets removed (keep first).
- **`ID_TYPE`/`ID_USB_TYPE`** from the invoked interface's `bInterfaceClass`: 01→audio, 03→hid, 06→media, 07→printer, 09→hub, 0e→video, e0→wireless, other→generic; 08→(subclass 02→cd, 03→tape, 04/07→floppy, else→disk).
- **Commit trailers:** end commit messages with the repo's two trailer lines (`Co-Authored-By:` and `Claude-Session:`).

## File Structure

- **Create `usb_id.h`** — the builtin: encoders, sysfs helpers specific to usb, type map, interface enumeration, `usb_id_build`.
- **Create `tests/test_usb_id.c`** — `assert`-based unit tests over synthetic sysfs trees.
- **Create `tests/verify_usb_id_live.sh`** — live acceptance harness (Claire's 0-mismatch gate).
- **Modify `Makefile`** — one `test:` line for `tests/test_usb_id.c`.
- **`schema-udev.c`** — untouched.

---

### Task 1: Scaffold `usb_id.h` + the string encoders

**Files:**
- Create: `usb_id.h`
- Create: `tests/test_usb_id.c`
- Modify: `Makefile` (the `test:` target, after the `test_path_id.c` line)

**Interfaces:**
- Consumes: `path_id.h` (for the transitive `schema-udev.h`; no pi_* used yet).
- Produces:
  - `int usb_in_safe(unsigned char c)` → 1 if c ∈ SAFE.
  - `void usb_replace_whitespace(const char *in, char *out, size_t outsz)`
  - `void usb_replace_chars(char *s)` (in place)
  - `void usb_plain(const char *in, char *out, size_t outsz)` (whitespace then chars)
  - `void usb_encode(const char *in, char *out, size_t outsz)`
  - `#define USB_STR_MAX 256`

- [ ] **Step 1: Write the failing test**

Create `tests/test_usb_id.c`:

```c
#include "../usb_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_path_id.c) --- */
static void mkdirp(const char *path) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    assert(system(cmd) == 0);
}
static void mkfile(const char *path, const char *content) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}
static void mklink(const char *linkpath, const char *target) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", linkpath);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    unlink(linkpath);
    assert(symlink(target, linkpath) == 0);
}

static void test_encoders(void) {
    char out[USB_STR_MAX];

    usb_plain("USB OPTICAL MOUSE ", out, sizeof out);
    assert(strcmp(out, "USB_OPTICAL_MOUSE") == 0);

    usb_plain("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic_Technology_Co.__Ltd.") == 0);

    usb_plain("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion") == 0);

    usb_encode("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);

    usb_encode("Seagate ", out, sizeof out);
    assert(strcmp(out, "Seagate\\x20") == 0);

    usb_encode("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion\\x20\\x20\\x20\\x20\\x20\\x20\\x20") == 0);

    /* SAFE chars survive both forms */
    usb_plain("7.0.12-cachyos1 x86_64", out, sizeof out);
    assert(strcmp(out, "7.0.12-cachyos1_x86_64") == 0);
    usb_encode("a:b-c.d", out, sizeof out);
    assert(strcmp(out, "a:b-c.d") == 0);

    printf("test_encoders OK\n");
}

int main(void) {
    test_encoders();
    printf("ALL usb_id tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid`
Expected: FAIL — `usb_id.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `usb_id.h`:

```c
#ifndef USB_ID_H
#define USB_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <ctype.h>

#define USB_STR_MAX 256

static inline int usb_in_safe(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '#' || c == '+' || c == '-' || c == '.'
        || c == ':' || c == '=' || c == '@' || c == '_';
}

static inline void usb_replace_whitespace(const char *in, char *out, size_t outsz) {
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) len--;   /* trim trailing */
    size_t i = 0;
    while (i < len && isspace((unsigned char)in[i])) i++;           /* skip leading */
    size_t j = 0;
    while (i < len && j + 1 < outsz) {
        if (isspace((unsigned char)in[i])) {
            while (i < len && isspace((unsigned char)in[i])) i++;
            out[j++] = '_';
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

static inline void usb_replace_chars(char *s) {
    for (; *s; s++)
        if (!usb_in_safe((unsigned char)*s)) *s = '_';
}

static inline void usb_plain(const char *in, char *out, size_t outsz) {
    usb_replace_whitespace(in, out, outsz);
    usb_replace_chars(out);
}

static inline void usb_encode(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (usb_in_safe(*p)) {
            if (j + 1 >= outsz) break;
            out[j++] = (char)*p;
        } else {
            if (j + 4 >= outsz) break;              /* "\xNN" = 4 chars + NUL */
            j += (size_t)snprintf(out + j, outsz - j, "\\x%02x", *p);
        }
    }
    out[j] = '\0';
}

#endif /* USB_ID_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: PASS — `test_encoders OK`, `ALL usb_id tests passed`.

- [ ] **Step 5: Wire the Makefile**

Add this line to the `test:` target immediately after the `tests/test_path_id.c` line:

```make
	$(CC) $(CFLAGS) tests/test_usb_id.c -o /tmp/schema-test-usbid && /tmp/schema-test-usbid
```

Run: `make test` — expected: all existing tests plus `ALL usb_id tests passed`.

- [ ] **Step 6: Commit**

```bash
git add usb_id.h tests/test_usb_id.c Makefile
git commit -m "feat(usb_id): scaffold builtin header + string encoders"
```

---

### Task 2: Node discovery + descriptor name fields + fallbacks

**Files:**
- Modify: `usb_id.h`
- Modify: `tests/test_usb_id.c`

**Interfaces:**
- Consumes: `pi_sysattr`, `pi_base`, `pi_parent`, `pi_subsystem` (from path_id.h); the encoders (Task 1).
- Produces:
  - `int usb_find_nodes(const char *sysroot, const char *devpath, char *devdir, size_t devsz, char *ifdir, size_t ifsz)` → 0 with `devdir` = absolute usb_device dir and `ifdir` = absolute usb_interface dir (or empty string if the invoker is not on/under an interface); −1 if no usb_device found.
  - `void usb_name_field(const char *devdir, const char *attr, const char *hexfallback, char *plain, size_t psz, char *enc, size_t esz)` → fills plain + enc forms of `<devdir>/<attr>`, or the hex fallback (both forms) when the attr is missing/empty.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_usb_id.c` and call from `main`:

```c
/* Build a minimal usb_device + interface + a leaf under the interface.
 * Returns via out_leaf the relative devpath of the leaf. */
static void build_usb_dev(const char *root, const char *pci_rel,
                          const char *devname,     /* e.g. "1-4" */
                          const char *ifname,       /* e.g. "1-4:1.0" */
                          const char *manufacturer, /* NULL to omit */
                          const char *product,      /* NULL to omit */
                          const char *serial,       /* NULL to omit */
                          const char *vid, const char *pid, const char *bcd,
                          char *out_leaf, size_t leafsz) {
    char utgt[2100], dir[2600], sub[2700], f[2800];
    snprintf(utgt, sizeof utgt, "%s/bus/usb", root); mkdirp(utgt);

    /* usb_device dir */
    char devrel[1024];
    snprintf(devrel, sizeof devrel, "%s/%s", pci_rel, devname);
    snprintf(dir, sizeof dir, "%s%s", root, devrel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);
    snprintf(f, sizeof f, "%s/idVendor", dir); mkfile(f, vid);
    snprintf(f, sizeof f, "%s/idProduct", dir); mkfile(f, pid);
    snprintf(f, sizeof f, "%s/bcdDevice", dir); mkfile(f, bcd);
    if (manufacturer) { snprintf(f, sizeof f, "%s/manufacturer", dir); mkfile(f, manufacturer); }
    if (product)      { snprintf(f, sizeof f, "%s/product", dir);      mkfile(f, product); }
    if (serial)       { snprintf(f, sizeof f, "%s/serial", dir);       mkfile(f, serial); }

    /* usb_interface dir */
    char ifrel[1200];
    snprintf(ifrel, sizeof ifrel, "%s/%s", devrel, ifname);
    snprintf(dir, sizeof dir, "%s%s", root, ifrel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, utgt);
    snprintf(f, sizeof f, "%s/bInterfaceNumber", dir); mkfile(f, "00\n");
    snprintf(f, sizeof f, "%s/bInterfaceClass", dir); mkfile(f, "0e\n");
    snprintf(f, sizeof f, "%s/bInterfaceSubClass", dir); mkfile(f, "01\n");
    snprintf(f, sizeof f, "%s/bInterfaceProtocol", dir); mkfile(f, "00\n");

    /* leaf under the interface (subsystem=video4linux) */
    char vtgt[2100];
    snprintf(vtgt, sizeof vtgt, "%s/class/video4linux", root); mkdirp(vtgt);
    char leafrel[1400];
    snprintf(leafrel, sizeof leafrel, "%s/video4linux/video0", ifrel);
    snprintf(dir, sizeof dir, "%s%s", root, leafrel); mkdirp(dir);
    snprintf(sub, sizeof sub, "%s/subsystem", dir); mklink(sub, vtgt);

    safe_copy(out_leaf, leafrel, leafsz);
}

static void test_discovery_and_names(void) {
    char tmpl[] = "/tmp/schema-usbid-d-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    char leaf[2048];
    build_usb_dev(root, "/devices/pci0000:00/0000:02:00.0/usb1", "1-4", "1-4:1.0",
                  "GenesysLogic Technology Co., Ltd.", "USB2.0 UVC PC Camera", NULL,
                  "a16f\n", "0304\n", "0620\n", leaf, sizeof leaf);

    char devdir[PATH_MAX], ifdir[PATH_MAX];
    assert(usb_find_nodes(root, leaf, devdir, sizeof devdir, ifdir, sizeof ifdir) == 0);
    assert(strcmp(pi_base(devdir), "1-4") == 0);
    assert(strcmp(pi_base(ifdir), "1-4:1.0") == 0);

    char plain[USB_STR_MAX], enc[USB_STR_MAX];
    usb_name_field(devdir, "manufacturer", "a16f", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "GenesysLogic_Technology_Co.__Ltd.") == 0);
    assert(strcmp(enc, "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);

    /* fallback: no manufacturer -> both forms = idVendor hex */
    char tmpl2[] = "/tmp/schema-usbid-f-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    char leaf2[2048];
    build_usb_dev(root2, "/devices/pci0000:00/0000:02:00.0/usb1", "1-5", "1-5:1.0",
                  NULL, "USB OPTICAL MOUSE ", NULL, "18f8\n", "0f99\n", "0100\n",
                  leaf2, sizeof leaf2);
    char devdir2[PATH_MAX], ifdir2[PATH_MAX];
    assert(usb_find_nodes(root2, leaf2, devdir2, sizeof devdir2, ifdir2, sizeof ifdir2) == 0);
    usb_name_field(devdir2, "manufacturer", "18f8", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "18f8") == 0);
    assert(strcmp(enc, "18f8") == 0);
    usb_name_field(devdir2, "product", "0f99", plain, sizeof plain, enc, sizeof enc);
    assert(strcmp(plain, "USB_OPTICAL_MOUSE") == 0);

    printf("test_discovery_and_names OK\n");
}
```

Add to `main`: `test_discovery_and_names();`

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: FAIL — `usb_find_nodes` / `usb_name_field` undefined.

- [ ] **Step 3: Write the implementation**

Add to `usb_id.h` before `#endif`:

```c
static inline int usb_find_nodes(const char *sysroot, const char *devpath,
                                 char *devdir, size_t devsz, char *ifdir, size_t ifsz) {
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur) return -1;
    devdir[0] = '\0'; ifdir[0] = '\0';
    char sub[128];
    for (;;) {
        if (pi_subsystem(cur, sub, sizeof sub) == 0 && strcmp(sub, "usb") == 0) {
            const char *b = pi_base(cur);
            if (strchr(b, ':')) {                 /* usb_interface */
                if (ifdir[0] == '\0') safe_copy(ifdir, cur, ifsz);
            } else {                              /* usb_device */
                safe_copy(devdir, cur, devsz);
                return 0;
            }
        }
        if (pi_parent(cur) != 0) break;
    }
    return devdir[0] ? 0 : -1;
}

static inline void usb_name_field(const char *devdir, const char *attr,
                                  const char *hexfallback,
                                  char *plain, size_t psz, char *enc, size_t esz) {
    char raw[USB_STR_MAX];
    if (pi_sysattr(devdir, attr, raw, sizeof raw) != 0 || raw[0] == '\0') {
        safe_copy(plain, hexfallback, psz);
        safe_copy(enc, hexfallback, esz);
        return;
    }
    usb_plain(raw, plain, psz);
    usb_encode(raw, enc, esz);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: PASS — `test_discovery_and_names OK`.

- [ ] **Step 5: Commit**

```bash
git add usb_id.h tests/test_usb_id.c
git commit -m "feat(usb_id): node discovery + descriptor name fields with fallbacks"
```

---

### Task 3: Type map, interface driver, and `ID_USB_INTERFACES`

**Files:**
- Modify: `usb_id.h`
- Modify: `tests/test_usb_id.c`

**Interfaces:**
- Consumes: `pi_sysattr`, `pi_base` (path_id.h).
- Produces:
  - `const char *usb_type_from_iface(const char *ifdir)` → the type string per the class/subclass map; `"generic"` if class unreadable.
  - `int usb_driver(const char *ifdir, char *out, size_t outsz)` → 0 with basename of `<ifdir>/driver` symlink, −1 if absent.
  - `void usb_interfaces(const char *devdir, char *out, size_t outsz)` → the `:`-wrapped, number-ordered, deduped triplet list.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_usb_id.c` and call from `main`:

```c
static void mk_iface(const char *root, const char *ifrel,
                     const char *num, const char *cls, const char *sub, const char *pro,
                     const char *driver) {
    char utgt[2100], dir[2600], link[2700], f[2800];
    snprintf(utgt, sizeof utgt, "%s/bus/usb", root); mkdirp(utgt);
    snprintf(dir, sizeof dir, "%s%s", root, ifrel); mkdirp(dir);
    snprintf(link, sizeof link, "%s/subsystem", dir); mklink(link, utgt);
    snprintf(f, sizeof f, "%s/bInterfaceNumber", dir); mkfile(f, num);
    snprintf(f, sizeof f, "%s/bInterfaceClass", dir); mkfile(f, cls);
    snprintf(f, sizeof f, "%s/bInterfaceSubClass", dir); mkfile(f, sub);
    snprintf(f, sizeof f, "%s/bInterfaceProtocol", dir); mkfile(f, pro);
    if (driver) {
        char dtgt[2200];
        snprintf(dtgt, sizeof dtgt, "%s/bus/usb/drivers/%s", root, driver); mkdirp(dtgt);
        snprintf(link, sizeof link, "%s/driver", dir); mklink(link, dtgt);
    }
}

static void test_type_driver_interfaces(void) {
    char tmpl[] = "/tmp/schema-usbid-t-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    const char *devrel = "/devices/pci0000:00/0000:02:00.0/usb1/1-7";
    char devdir[PATH_MAX];
    snprintf(devdir, sizeof devdir, "%s%s", root, devrel);
    mkdirp(devdir);

    /* pico key: ifaces 0,1,2,3 = 030000, 030000(dup), 0b0000, ff0000 */
    char ifrel[1200];
    snprintf(ifrel, sizeof ifrel, "%s/1-7:1.0", devrel); mk_iface(root, ifrel, "00\n","03\n","00\n","00\n","usbhid");
    snprintf(ifrel, sizeof ifrel, "%s/1-7:1.1", devrel); mk_iface(root, ifrel, "01\n","03\n","00\n","00\n",NULL);
    snprintf(ifrel, sizeof ifrel, "%s/1-7:1.2", devrel); mk_iface(root, ifrel, "02\n","0b\n","00\n","00\n",NULL);
    snprintf(ifrel, sizeof ifrel, "%s/1-7:1.3", devrel); mk_iface(root, ifrel, "03\n","ff\n","00\n","00\n",NULL);

    char out[USB_STR_MAX];
    usb_interfaces(devdir, out, sizeof out);
    assert(strcmp(out, ":030000:0b0000:ff0000:") == 0);   /* number order + dedup */

    /* type from the class-0e interface */
    char ifdir[PATH_MAX];
    snprintf(ifdir, sizeof ifdir, "%s%s/1-7:1.0", root, devrel);
    assert(strcmp(usb_type_from_iface(ifdir), "hid") == 0);

    char drv[64];
    assert(usb_driver(ifdir, drv, sizeof drv) == 0);
    assert(strcmp(drv, "usbhid") == 0);

    /* mouse ordering (not value-sorted): ifaces 0=030102, 1=030101 */
    char tmpl2[] = "/tmp/schema-usbid-m-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    const char *drel2 = "/devices/pci0000:00/0000:02:00.0/usb1/1-5";
    char dd2[PATH_MAX]; snprintf(dd2, sizeof dd2, "%s%s", root2, drel2); mkdirp(dd2);
    snprintf(ifrel, sizeof ifrel, "%s/1-5:1.0", drel2); mk_iface(root2, ifrel, "00\n","03\n","01\n","02\n",NULL);
    snprintf(ifrel, sizeof ifrel, "%s/1-5:1.1", drel2); mk_iface(root2, ifrel, "01\n","03\n","01\n","01\n",NULL);
    usb_interfaces(dd2, out, sizeof out);
    assert(strcmp(out, ":030102:030101:") == 0);

    /* type map: mass storage class 08 subclass 06 -> disk */
    char tmpl3[] = "/tmp/schema-usbid-s-XXXXXX";
    char *root3 = mkdtemp(tmpl3);
    assert(root3);
    const char *sifrel = "/devices/pci0000:00/x/4-1/4-1:1.0";
    mk_iface(root3, sifrel, "00\n","08\n","06\n","50\n","usb-storage");
    char sifdir[PATH_MAX]; snprintf(sifdir, sizeof sifdir, "%s%s", root3, sifrel);
    assert(strcmp(usb_type_from_iface(sifdir), "disk") == 0);

    printf("test_type_driver_interfaces OK\n");
}
```

Add to `main`: `test_type_driver_interfaces();`

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: FAIL — the three functions undefined.

- [ ] **Step 3: Write the implementation**

Add to `usb_id.h` before `#endif`:

```c
static inline const char *usb_type_from_iface(const char *ifdir) {
    char cls[16], sub[16];
    if (pi_sysattr(ifdir, "bInterfaceClass", cls, sizeof cls) != 0) return "generic";
    if (strcmp(cls, "01") == 0) return "audio";
    if (strcmp(cls, "03") == 0) return "hid";
    if (strcmp(cls, "06") == 0) return "media";
    if (strcmp(cls, "07") == 0) return "printer";
    if (strcmp(cls, "08") == 0) {
        if (pi_sysattr(ifdir, "bInterfaceSubClass", sub, sizeof sub) == 0) {
            if (strcmp(sub, "02") == 0) return "cd";
            if (strcmp(sub, "03") == 0) return "tape";
            if (strcmp(sub, "04") == 0 || strcmp(sub, "07") == 0) return "floppy";
        }
        return "disk";
    }
    if (strcmp(cls, "09") == 0) return "hub";
    if (strcmp(cls, "0e") == 0) return "video";
    if (strcmp(cls, "e0") == 0) return "wireless";
    return "generic";
}

static inline int usb_driver(const char *ifdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/driver", ifdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

struct usb_if { int num; char trip[8]; };

static inline void usb_interfaces(const char *devdir, char *out, size_t outsz) {
    const char *devbase = pi_base(devdir);
    char prefix[128];
    snprintf(prefix, sizeof prefix, "%s:", devbase);
    size_t plen = strlen(prefix);

    struct usb_if ifs[32];
    int n = 0;
    DIR *d = opendir(devdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 32) {
            if (strncmp(e->d_name, prefix, plen) != 0) continue;
            char ifp[PATH_MAX];
            if ((size_t)snprintf(ifp, sizeof ifp, "%s/%s", devdir, e->d_name) >= sizeof ifp) continue;
            char cls[8], sub[8], pro[8], num[8];
            if (pi_sysattr(ifp, "bInterfaceClass", cls, sizeof cls) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceSubClass", sub, sizeof sub) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceProtocol", pro, sizeof pro) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceNumber", num, sizeof num) != 0) continue;
            ifs[n].num = (int)strtol(num, NULL, 16);
            snprintf(ifs[n].trip, sizeof ifs[n].trip, "%s%s%s", cls, sub, pro);
            n++;
        }
        closedir(d);
    }
    /* insertion sort ascending by bInterfaceNumber */
    for (int i = 1; i < n; i++) {
        struct usb_if key = ifs[i];
        int j = i - 1;
        while (j >= 0 && ifs[j].num > key.num) { ifs[j + 1] = ifs[j]; j--; }
        ifs[j + 1] = key;
    }
    /* build ":t:t:...:" with dedup */
    char buf[USB_STR_MAX];
    size_t bl = 0;
    buf[bl++] = ':'; buf[bl] = '\0';
    for (int i = 0; i < n; i++) {
        char probe[16];
        snprintf(probe, sizeof probe, ":%s:", ifs[i].trip);
        if (strstr(buf, probe)) continue;              /* dedup: triplet already present */
        char seg[16];
        int w = snprintf(seg, sizeof seg, "%s:", ifs[i].trip);
        if (w > 0 && bl + (size_t)w < sizeof buf) {
            memcpy(buf + bl, seg, (size_t)w);
            bl += (size_t)w;
            buf[bl] = '\0';
        }
    }
    safe_copy(out, buf, outsz);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: PASS — `test_type_driver_interfaces OK`.

- [ ] **Step 5: Commit**

```bash
git add usb_id.h tests/test_usb_id.c
git commit -m "feat(usb_id): type map, interface driver, ID_USB_INTERFACES"
```

---

### Task 4: Orchestrator `usb_id_build` (serial composition + full key set)

**Files:**
- Modify: `usb_id.h`
- Modify: `tests/test_usb_id.c`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces:
  - `int usb_id_build(const char *sysroot, const char *devpath, struct uevent *out)` → 0 with `out` populated (unprefixed keys, then `ID_USB_`-prefixed duplicates, then interfaces/num/driver); −1 if the device is not USB (no `idVendor`).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_usb_id.c` and call from `main`. Reuse `build_usb_dev` from Task 2 (camera, no serial), then assert the complete key set:

```c
static const char *ev_get(const struct uevent *ev, const char *k) {
    for (int i = 0; i < ev->n; i++) if (strcmp(ev->key[i], k) == 0) return ev->val[i];
    return NULL;
}

static void test_build_full(void) {
    char tmpl[] = "/tmp/schema-usbid-b-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);
    char leaf[2048];
    /* camera: manufacturer + product, NO serial; interface 1-4:1.0 class 0e (video) */
    build_usb_dev(root, "/devices/pci0000:00/0000:02:00.0/usb1", "1-4", "1-4:1.0",
                  "GenesysLogic Technology Co., Ltd.", "USB2.0 UVC PC Camera", NULL,
                  "a16f\n", "0304\n", "0620\n", leaf, sizeof leaf);
    /* add a second interface so ID_USB_INTERFACES has two entries like the real camera.
     * build_usb_dev's interface is class 0e sub 01 -> 0e0100; this second is 0e0200 */
    mk_iface(root, "/devices/pci0000:00/0000:02:00.0/usb1/1-4/1-4:1.1",
             "01\n", "0e\n", "02\n", "00\n", NULL);
    /* set the interface driver so ID_USB_DRIVER is present */
    {
        char dtgt[2200], link[2600];
        snprintf(dtgt, sizeof dtgt, "%s/bus/usb/drivers/uvcvideo", root); mkdirp(dtgt);
        snprintf(link, sizeof link, "%s/devices/pci0000:00/0000:02:00.0/usb1/1-4/1-4:1.0/driver", root);
        mklink(link, dtgt);
    }

    struct uevent ev;
    assert(usb_id_build(root, leaf, &ev) == 0);

    assert(strcmp(ev_get(&ev, "ID_BUS"), "usb") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR_ID"), "a16f") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL_ID"), "0304") == 0);
    assert(strcmp(ev_get(&ev, "ID_REVISION"), "0620") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR"), "GenesysLogic_Technology_Co.__Ltd.") == 0);
    assert(strcmp(ev_get(&ev, "ID_VENDOR_ENC"), "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL"), "USB2.0_UVC_PC_Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_MODEL_ENC"), "USB2.0\\x20UVC\\x20PC\\x20Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_SERIAL"),
                  "GenesysLogic_Technology_Co.__Ltd._USB2.0_UVC_PC_Camera") == 0);
    assert(ev_get(&ev, "ID_SERIAL_SHORT") == NULL);          /* no serial sysattr */
    assert(strcmp(ev_get(&ev, "ID_TYPE"), "video") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_MODEL"), "USB2.0_UVC_PC_Camera") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_TYPE"), "video") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_INTERFACES"), ":0e0100:0e0200:") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_INTERFACE_NUM"), "00") == 0);
    assert(strcmp(ev_get(&ev, "ID_USB_DRIVER"), "uvcvideo") == 0);

    /* with a serial: composition includes _SHORT */
    char tmpl2[] = "/tmp/schema-usbid-b2-XXXXXX";
    char *root2 = mkdtemp(tmpl2);
    assert(root2);
    char leaf2[2048];
    build_usb_dev(root2, "/devices/pci0000:00/0000:02:00.0/usb1", "1-7", "1-7:1.0",
                  "Pol Henarejos", "Pico Key", "44BA59F930300000",
                  "2e8a\n", "10fe\n", "0806\n", leaf2, sizeof leaf2);
    struct uevent ev2;
    assert(usb_id_build(root2, leaf2, &ev2) == 0);
    assert(strcmp(ev_get(&ev2, "ID_SERIAL"), "Pol_Henarejos_Pico_Key_44BA59F930300000") == 0);
    assert(strcmp(ev_get(&ev2, "ID_SERIAL_SHORT"), "44BA59F930300000") == 0);
    assert(strcmp(ev_get(&ev2, "ID_USB_SERIAL_SHORT"), "44BA59F930300000") == 0);

    printf("test_build_full OK\n");
}
```

Add to `main`: `test_build_full();`

> Implementer note: the line building `1-4:1.1` uses `mk_iface` directly; the odd `snprintf(..., 0 ? "" : "")` / `(void)ifrel;` lines are scratch — delete them and keep only the `mk_iface(root, ".../1-4:1.1", "01\n","0e\n","02\n","00\n", NULL);` call and the driver-symlink block. The asserts are authoritative.

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: FAIL — `usb_id_build` undefined.

- [ ] **Step 3: Write the implementation**

Add to `usb_id.h` before `#endif`:

```c
static inline int usb_id_build(const char *sysroot, const char *devpath, struct uevent *out) {
    char devdir[PATH_MAX], ifdir[PATH_MAX];
    if (usb_find_nodes(sysroot, devpath, devdir, sizeof devdir, ifdir, sizeof ifdir) != 0)
        return -1;

    char vid[16], pid[16], rev[16];
    if (pi_sysattr(devdir, "idVendor", vid, sizeof vid) != 0) return -1;   /* not USB */
    if (pi_sysattr(devdir, "idProduct", pid, sizeof pid) != 0) return -1;
    if (pi_sysattr(devdir, "bcdDevice", rev, sizeof rev) != 0) rev[0] = '\0';

    char vendor[USB_STR_MAX], vendor_enc[USB_STR_MAX];
    char model[USB_STR_MAX], model_enc[USB_STR_MAX];
    usb_name_field(devdir, "manufacturer", vid, vendor, sizeof vendor, vendor_enc, sizeof vendor_enc);
    usb_name_field(devdir, "product", pid, model, sizeof model, model_enc, sizeof model_enc);

    char serial_short[USB_STR_MAX];
    int have_serial = 0;
    {
        char raw[USB_STR_MAX];
        if (pi_sysattr(devdir, "serial", raw, sizeof raw) == 0 && raw[0]) {
            usb_plain(raw, serial_short, sizeof serial_short);
            have_serial = 1;
        }
    }

    char serial[USB_STR_MAX * 3];
    if (have_serial) snprintf(serial, sizeof serial, "%s_%s_%s", vendor, model, serial_short);
    else             snprintf(serial, sizeof serial, "%s_%s", vendor, model);

    const char *type = ifdir[0] ? usb_type_from_iface(ifdir) : "generic";
    char ifaces[USB_STR_MAX];
    usb_interfaces(devdir, ifaces, sizeof ifaces);
    char ifnum[16];
    int have_ifnum = ifdir[0] && pi_sysattr(ifdir, "bInterfaceNumber", ifnum, sizeof ifnum) == 0;
    char drv[64];
    int have_drv = ifdir[0] && usb_driver(ifdir, drv, sizeof drv) == 0;

    out->n = 0;
    #define UEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    UEMIT("ID_BUS", "usb");
    UEMIT("ID_MODEL", model); UEMIT("ID_MODEL_ENC", model_enc); UEMIT("ID_MODEL_ID", pid);
    UEMIT("ID_SERIAL", serial);
    if (have_serial) UEMIT("ID_SERIAL_SHORT", serial_short);
    UEMIT("ID_VENDOR", vendor); UEMIT("ID_VENDOR_ENC", vendor_enc); UEMIT("ID_VENDOR_ID", vid);
    if (rev[0]) UEMIT("ID_REVISION", rev);
    UEMIT("ID_TYPE", type);

    UEMIT("ID_USB_MODEL", model); UEMIT("ID_USB_MODEL_ENC", model_enc); UEMIT("ID_USB_MODEL_ID", pid);
    UEMIT("ID_USB_SERIAL", serial);
    if (have_serial) UEMIT("ID_USB_SERIAL_SHORT", serial_short);
    UEMIT("ID_USB_VENDOR", vendor); UEMIT("ID_USB_VENDOR_ENC", vendor_enc); UEMIT("ID_USB_VENDOR_ID", vid);
    if (rev[0]) UEMIT("ID_USB_REVISION", rev);
    UEMIT("ID_USB_TYPE", type);
    UEMIT("ID_USB_INTERFACES", ifaces);
    if (have_ifnum) UEMIT("ID_USB_INTERFACE_NUM", ifnum);
    if (have_drv) UEMIT("ID_USB_DRIVER", drv);
    #undef UEMIT
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -Wall -Wextra -D_GNU_SOURCE tests/test_usb_id.c -o /tmp/t_usbid && /tmp/t_usbid`
Expected: PASS — `test_build_full OK`.

- [ ] **Step 5: Run the full suite + boundary gate**

Run: `make test` — expected all tests pass including `ALL usb_id tests passed`.
Run: `git diff master..HEAD -- schema-udev.c` — expected: empty.
Run: `grep -n usb_id schema-udev.c` — expected: no output.

- [ ] **Step 6: Commit**

```bash
git add usb_id.h tests/test_usb_id.c
git commit -m "feat(usb_id): orchestrator - serial composition + full property set"
```

---

### Task 5: Live acceptance harness + vmtest

**Files:**
- Create: `tests/verify_usb_id_live.sh`

**Interfaces:**
- Consumes: the full `usb_id.h`.
- Produces: a standalone read-only harness that runs `usb_id_build` over every live device with `ID_USB_VENDOR_ID` and diffs the usb_id-owned keys against real udev.

- [ ] **Step 1: Create the harness**

Create `tests/verify_usb_id_live.sh`:

```sh
#!/bin/sh
# Live parity gate: run usb_id_build over every /sys device real udev gave a
# USB identity (ID_USB_VENDOR_ID), diff emitted keys vs `udevadm info`.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/usbid_driver.c <<'EOF'
#include "usb_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (usb_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
cc -Wall -Wextra -D_GNU_SOURCE /tmp/usbid_driver.c -o /tmp/usbid_driver

props=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -q '^ID_USB_VENDOR_ID=' "$props" || continue
    total=$((total + 1))
    /tmp/usbid_driver "$devpath" | while IFS= read -r line; do
        key=${line%%=*}
        # unprefixed ID_TYPE is overwritten by scsi/ata on block nodes; ID_USB_TYPE is authoritative
        [ "$key" = "ID_TYPE" ] && continue
        grep -qxF "$line" "$props" || {
            udv=$(grep "^$key=" "$props" || echo "(absent)")
            printf 'MISMATCH %s\n  emit=%s\n  udev=%s\n' "$devpath" "$line" "$udv"
        }
    done >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'usb_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$misses"
[ "$miss" -eq 0 ]
```

Make it executable: `chmod +x tests/verify_usb_id_live.sh`

- [ ] **Step 2: Run the live gate (on blakbox)**

Run: `sh tests/verify_usb_id_live.sh`
Expected: `usb_id live parity: 41 devices, 0 mismatches` and exit 0.
Any MISMATCH block names the devpath + emitted vs udev value — fix the responsible function and re-run. Do not proceed with a nonzero mismatch count.

- [ ] **Step 3: vmtest (PID-1 rail regression)**

Run: `cd ~/schema-livetest && ./vmtest.sh`
Expected: `>> RESULT: PASS`. usb_id is not in the PID-1 path; this only confirms nothing else regressed.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_usb_id_live.sh
git commit -m "test(usb_id): live udev parity acceptance harness"
```

---

## Self-Review

**Spec coverage:**
- Two encoders (plain trim+collapse, ENC per-char no-trim, SAFE set) → Task 1. ✓
- Node discovery (usb_device + usb_interface) → Task 2. ✓
- Descriptor fields + fallbacks (manufacturer/product → idVendor/idProduct hex) → Task 2. ✓
- Serial composition (+ ID_SERIAL_SHORT when present) → Task 4. ✓
- ID_TYPE class map + 08 subclass refinement → Task 3. ✓
- ID_USB_INTERFACES (number order + dedup) → Task 3. ✓
- ID_USB_INTERFACE_NUM + ID_USB_DRIVER → Tasks 3 (driver) + 4 (assembly). ✓
- Full key set incl ID_USB_ duplicates → Task 4. ✓
- schema-udev.c byte-identical boundary → Task 4 Step 5. ✓
- Live 0-mismatch across the 41 → Task 5. ✓

**Type consistency:** `usb_in_safe`, `usb_replace_whitespace`, `usb_replace_chars`, `usb_plain`, `usb_encode`, `usb_find_nodes`, `usb_name_field`, `usb_type_from_iface`, `usb_driver`, `usb_interfaces`, `usb_id_build`, `struct usb_if` — signatures identical everywhere referenced. `USB_STR_MAX`=256 consistent. Reused path_id.h helpers (`pi_sysattr`/`pi_base`/`pi_parent`/`pi_subsystem`) match their landed signatures.

**Placeholder scan:** No TBD/TODO, no scratch code. All test code is clean and directly compilable; the asserted values are the authoritative spec.

## Notes for the verifier (Claire)

- **`\xNN` in C test literals:** the expected string for udev's `\x20` is written `"\\x20"` in the C source. Confirm the tests assert the escaped-backslash form (that is what lands in `/run/udev/data`).
- The unit tests fabricate sysfs; the **live harness (Task 5) is the real gate** — re-run it on blakbox, require 0 mismatches across all 41 `ID_USB_VENDOR_ID` devices.
- Watch the two subtle cases the unit tests encode but that only the live box stresses at scale: the **plain-vs-ENC trailing-whitespace asymmetry** (Seagate/Expansion) and **`ID_USB_INTERFACES` ordering+dedup** (a multi-interface device with a duplicate triplet and non-monotonic triplet values).
- Boundary: `git diff master..HEAD -- schema-udev.c` empty and `grep usb_id schema-udev.c` empty.
