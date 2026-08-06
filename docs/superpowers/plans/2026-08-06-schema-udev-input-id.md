# schema-udev input_id builtin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce udev's `input_id` builtin — synthesize `ID_INPUT`/`ID_INPUT_*` from an input device's sysfs capability bitmasks — byte-for-byte across the 41 input devices on blakbox, with the full udev algorithm (all device classes).

**Architecture:** New header-only `input_id.h`, a faithful port of systemd `src/udev/udev-builtin-input_id.c`. It parses the reversed-hex-word capability bitmasks, resolves the `inputN` node by walking sysfs, runs udev's `test_pointers` + `test_key` heuristic, and emits properties into a `struct uevent`. Mechanism only — wired to nothing. `schema-udev.c` stays byte-identical.

**Tech Stack:** C99, `-O2 -Wall -Wextra -D_GNU_SOURCE`, GNU Make. Reuses `path_id.h` (`pi_sysattr`, `pi_parent`, `pi_base`, `safe_copy`) and `schema-udev.h` (`struct uevent`, `uevent_get`, `UE_MAX_KEYS`).

## Global Constraints

- **Boundary:** `schema-udev.c` and `schema-udev.h` MUST remain byte-identical to master. `grep input_id schema-udev.c` MUST be empty. The builtin is off by default, wired to nothing.
- **Normative source:** systemd `src/udev/udev-builtin-input_id.c`. Where this plan and that source disagree, the source governs and the live gate is the authority.
- **Emit only `=1`.** Never emit `ID_INPUT_MOUSE=0`. Absence == false. Values are always the literal string `"1"`.
- **Bitmask encoding:** each `capabilities/*` field is space-separated hex longs, **most-significant word first, word0 is the rightmost token.** Parse into a zero-filled `unsigned long[IID_NWORDS]`; a field may print fewer words than the array holds. `BITS_PER_LONG = 64` (x86-64 target).
- **Sysfs layout:** masks are on the `inputN` node. `ev/key/rel/abs` live under `capabilities/`; the `INPUT_PROP_*` mask lives at `<inputN>/properties` — a **sibling of `capabilities/`, NOT `capabilities/prop`.**
- **`input_id.h` includes `path_id.h`** (transitively `schema-udev.h`) — do not re-include or re-implement its primitives (DRY).
- Reuse the `iid_emit(out, k, v)` helper for every property; guard on `out->n < UE_MAX_KEYS`.
- Full-line exact match is the parity standard, **both directions** (wrong value AND under-emission).

---

### Task 1: `input_id.h` scaffold — bitmask parse + `test_bit`

**Files:**
- Create: `input_id.h`
- Test: `tests/test_input_id.c`
- Modify: `Makefile` (add the test build line)

**Interfaces:**
- Consumes: `path_id.h` (`safe_copy`), `schema-udev.h` (`struct uevent`, `UE_MAX_KEYS`, `UE_KEY_MAX`, `UE_VAL_MAX`, `uevent_get`).
- Produces:
  - `#define IID_NWORDS 12` (12×64 = 768 bits ≥ `KEY_MAX+1 = 0x300`).
  - `void iid_parse_mask(const char *s, unsigned long arr[IID_NWORDS])` — zero-fills, then fills word `k-1-i` from left-token `i` of `k` tokens; skips out-of-range words.
  - `int iid_test_bit(const unsigned long arr[IID_NWORDS], unsigned n)`.
  - `int iid_any_bit(const unsigned long arr[IID_NWORDS], unsigned lo, unsigned hi)` — any bit set in `[lo, hi)`.
  - `void iid_emit(struct uevent *out, const char *k, const char *v)`.
  - The input-event-code `#define`s (see Step 3).

- [ ] **Step 1: Write the failing test**

Create `tests/test_input_id.c`:

```c
#include "input_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mask(void) {
    unsigned long a[IID_NWORDS];

    /* single word: word0 = rightmost */
    iid_parse_mask("8000", a);
    assert(iid_test_bit(a, 15));           /* 0x8000 = bit 15 */
    assert(!iid_test_bit(a, 14));

    /* multi-word: MSW first. "1f0000 0 0 0 0" -> word4 = 0x1f0000 */
    iid_parse_mask("1f0000 0 0 0 0", a);
    assert(iid_test_bit(a, 4*64 + 16));    /* BTN_LEFT = 0x110 = 272 */
    assert(iid_test_bit(a, 4*64 + 20));    /* 0x114 = 276 */
    assert(!iid_test_bit(a, 4*64 + 21));

    /* keyboard word0 mask */
    iid_parse_mask("fffffffffffffffe", a);
    assert((a[0] & 0xFFFFFFFEUL) == 0xFFFFFFFEUL);
    assert(!iid_test_bit(a, 0));
    assert(iid_test_bit(a, 1));

    /* short field: high words zero, no OOB */
    iid_parse_mask("0", a);
    assert(!iid_test_bit(a, 0));
    assert(!iid_test_bit(a, 700));

    /* NULL safe */
    iid_parse_mask(NULL, a);
    assert(!iid_test_bit(a, 0));

    /* any_bit range */
    iid_parse_mask("10000 0 0 0", a);      /* word3 bit16 = 3*64+16 = 208 */
    assert(iid_any_bit(a, 200, 220));
    assert(!iid_any_bit(a, 0, 200));

    printf("test_mask OK\n");
}

int main(void) {
    test_mask();
    printf("ALL input_id tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `input_id.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `input_id.h`:

```c
#ifndef SCHEMA_INPUT_ID_H
#define SCHEMA_INPUT_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- input-event-codes.h subset (kernel headers not included) --- */
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_SW  0x05
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RX 0x03
#define ABS_PRESSURE 0x18
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08
#define BTN_MISC 0x100
#define BTN_MOUSE 0x110
#define BTN_JOYSTICK 0x120
#define BTN_DIGI 0x140
#define BTN_TOOL_PEN 0x140
#define BTN_TOOL_FINGER 0x145
#define BTN_TOUCH 0x14a
#define BTN_STYLUS 0x14b
#define BTN_TRIGGER_HAPPY 0x2c0
#define BTN_TRIGGER_HAPPY40 0x2e7
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_POINTING_STICK 0x05
#define INPUT_PROP_ACCELEROMETER 0x06

#define IID_NWORDS 12   /* 12*64 = 768 bits >= KEY_MAX+1 (0x300) */

static inline void iid_parse_mask(const char *s, unsigned long arr[IID_NWORDS]) {
    for (int i = 0; i < IID_NWORDS; i++) arr[i] = 0;
    if (!s) return;
    char buf[1024], cnt[1024];
    safe_copy(buf, s, sizeof buf);
    safe_copy(cnt, s, sizeof cnt);
    int k = 0; char *sv = NULL;
    for (char *t = strtok_r(cnt, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv)) k++;
    int i = 0; sv = NULL;
    for (char *t = strtok_r(buf, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv), i++) {
        int w = k - 1 - i;
        if (w >= 0 && w < IID_NWORDS) arr[w] = strtoul(t, NULL, 16);
    }
}

static inline int iid_test_bit(const unsigned long arr[IID_NWORDS], unsigned n) {
    unsigned w = n / 64, b = n % 64;
    if (w >= IID_NWORDS) return 0;
    return (arr[w] >> b) & 1UL;
}

static inline int iid_any_bit(const unsigned long arr[IID_NWORDS], unsigned lo, unsigned hi) {
    for (unsigned b = lo; b < hi; b++)
        if (iid_test_bit(arr, b)) return 1;
    return 0;
}

static inline void iid_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_mask OK` / `ALL input_id tests passed`. No warnings.

- [ ] **Step 5: Add the Makefile test line**

In `Makefile`, in the `test:` target, after the `test_usb_id.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_input_id.c -o /tmp/schema-test-inputid && /tmp/schema-test-inputid
```

- [ ] **Step 6: Commit**

```bash
git add input_id.h tests/test_input_id.c Makefile
git commit -m "feat(input_id): scaffold builtin header + bitmask parser"
```

---

### Task 2: node resolution + mask reads

**Files:**
- Modify: `input_id.h`
- Test: `tests/test_input_id.c`

**Interfaces:**
- Consumes: `pi_sysattr`, `pi_parent`, `safe_copy` from `path_id.h`; `iid_parse_mask`.
- Produces:
  - `int iid_find_input_node(const char *sysroot, const char *devpath, char *out, size_t outsz)` — climbs from `sysroot+devpath` to the nearest ancestor dir containing `capabilities/ev`; returns 0 and writes the abs dir to `out`, or -1.
  - `int iid_read_masks(const char *inputdir, unsigned long ev[IID_NWORDS], unsigned long key[IID_NWORDS], unsigned long rel[IID_NWORDS], unsigned long abs_[IID_NWORDS], unsigned long prop[IID_NWORDS])` — reads all five (prop from `properties`, others from `capabilities/*`); missing/empty fields parse to all-zero. Returns 0.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_input_id.c` a fabricated-sysfs helper block and a test, then call it from `main` before the final print:

```c
#include <sys/stat.h>
#include <limits.h>

static void iid_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static void iid_wf(const char *dir, const char *name, const char *val) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(val, f); fputc('\n', f); fclose(f);
}

/* build <root><rel>/{capabilities/{ev,key,rel,abs},properties}; returns dir written into caps parent */
static void iid_make_node(const char *root, const char *rel,
                          const char *ev, const char *key, const char *rel_, const char *abs_, const char *prop) {
    char node[PATH_MAX], caps[PATH_MAX];
    snprintf(node, sizeof node, "%s%s", root, rel);
    snprintf(caps, sizeof caps, "%s/capabilities", node);
    iid_mkdirs(caps);
    iid_wf(caps, "ev", ev); iid_wf(caps, "key", key);
    iid_wf(caps, "rel", rel_); iid_wf(caps, "abs", abs_);
    iid_wf(node, "properties", prop);
}

static void test_discovery(void) {
    char root[] = "/tmp/iidtestXXXXXX";
    assert(mkdtemp(root));
    iid_make_node(root, "/devices/inp0", "3", "8000", "0", "0", "0");   /* word0 = 0x8000 */
    /* also create an eventN child dir to resolve up from */
    char evdir[PATH_MAX]; snprintf(evdir, sizeof evdir, "%s/devices/inp0/event0", root);
    iid_mkdirs(evdir);

    char found[PATH_MAX];
    assert(iid_find_input_node(root, "/devices/inp0/event0", found, sizeof found) == 0);
    char want[PATH_MAX]; snprintf(want, sizeof want, "%s/devices/inp0", root);
    assert(strcmp(found, want) == 0);

    unsigned long ev[IID_NWORDS], key[IID_NWORDS], rl[IID_NWORDS], ab[IID_NWORDS], pr[IID_NWORDS];
    assert(iid_read_masks(found, ev, key, rl, ab, pr) == 0);
    assert(iid_test_bit(ev, 0) && iid_test_bit(ev, 1));   /* ev=3 */
    assert(iid_test_bit(key, 15));                        /* 0x8000 word0 */
    assert(!iid_test_bit(pr, 0));

    /* non-input path resolves to failure */
    char miss[PATH_MAX];
    assert(iid_find_input_node(root, "/devices", miss, sizeof miss) != 0);
    printf("test_discovery OK\n");
}
```

Add `test_discovery();` to `main` before the final print.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `iid_find_input_node` / `iid_read_masks` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `input_id.h` before the `#endif`:

```c
static inline int iid_find_input_node(const char *sysroot, const char *devpath,
                                      char *out, size_t outsz) {
    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", sysroot, devpath) >= sizeof dir) return -1;
    for (;;) {
        char probe[PATH_MAX];
        if ((size_t)snprintf(probe, sizeof probe, "%s/capabilities/ev", dir) < sizeof probe
            && access(probe, R_OK) == 0) {
            safe_copy(out, dir, outsz);
            return 0;
        }
        if (pi_parent(dir) != 0) return -1;
    }
}

static inline int iid_read_masks(const char *inputdir,
                                 unsigned long ev[IID_NWORDS], unsigned long key[IID_NWORDS],
                                 unsigned long rel[IID_NWORDS], unsigned long abs_[IID_NWORDS],
                                 unsigned long prop[IID_NWORDS]) {
    char b[1024];
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/ev",  b, sizeof b) == 0 ? b : NULL, ev);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/key", b, sizeof b) == 0 ? b : NULL, key);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/rel", b, sizeof b) == 0 ? b : NULL, rel);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/abs", b, sizeof b) == 0 ? b : NULL, abs_);
    iid_parse_mask(pi_sysattr(inputdir, "properties",       b, sizeof b) == 0 ? b : NULL, prop);
    return 0;
}
```

Note: `pi_sysattr` reads one line and strips the trailing newline — capability fields are single-line, so this is exact.

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_discovery OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add input_id.h tests/test_input_id.c
git commit -m "feat(input_id): input-node resolution + capability mask reads"
```

---

### Task 3: `iid_test_key` + `iid_test_pointers` heuristic

**Files:**
- Modify: `input_id.h`
- Test: `tests/test_input_id.c`

**Interfaces:**
- Consumes: `iid_test_bit`, `iid_any_bit`, `iid_emit`, the event-code `#define`s.
- Produces:
  - `int iid_test_key(const unsigned long ev[IID_NWORDS], const unsigned long key[IID_NWORDS], struct uevent *out)` — emits `ID_INPUT_KEYBOARD` and/or `ID_INPUT_KEY`; returns 1 if either emitted.
  - `int iid_test_pointers(const unsigned long ev[IID_NWORDS], const unsigned long abs_[IID_NWORDS], const unsigned long key[IID_NWORDS], const unsigned long rel[IID_NWORDS], const unsigned long prop[IID_NWORDS], struct uevent *out)` — emits at most one pointer-family flag set; returns 1 if any matched.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_input_id.c`. Helper `has` checks emission:

```c
static int iid_has(const struct uevent *ev, const char *k) {
    const char *v = uevent_get(ev, k);
    return v && strcmp(v, "1") == 0;
}

static void test_heuristic(void) {
    unsigned long ev[IID_NWORDS], key[IID_NWORDS], rel[IID_NWORDS], ab[IID_NWORDS], pr[IID_NWORDS];
    struct uevent out;

    /* full keyboard: word0 = 0xfffffffffffffffe */
    iid_parse_mask("3", ev); iid_parse_mask("fffffffffffffffe", key);
    out.n = 0;
    assert(iid_test_key(ev, key, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_KEYBOARD") && iid_has(&out, "ID_INPUT_KEY"));

    /* key-only: a media key in word3, not a full keyboard */
    iid_parse_mask("3", ev); iid_parse_mask("100000 0 0 0", key);
    out.n = 0;
    assert(iid_test_key(ev, key, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_KEY") && !iid_has(&out, "ID_INPUT_KEYBOARD"));

    /* no EV_KEY -> nothing */
    iid_parse_mask("21", ev); iid_parse_mask("0", key);   /* ev = SYN|SW */
    out.n = 0;
    assert(iid_test_key(ev, key, &out) == 0);
    assert(out.n == 0);

    /* mouse: EV_KEY|EV_REL, BTN_MOUSE range set, REL_X/Y set */
    iid_parse_mask("17", ev);                     /* SYN|KEY|REL|MSC */
    iid_parse_mask("1f0000 0 0 0 0", key);        /* BTN_LEFT.. word4 */
    iid_parse_mask("3", rel);                     /* REL_X|REL_Y */
    iid_parse_mask("0", ab); iid_parse_mask("0", pr);
    out.n = 0;
    assert(iid_test_pointers(ev, ab, key, rel, pr, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_MOUSE"));
    assert(!iid_has(&out, "ID_INPUT_TOUCHPAD") && !iid_has(&out, "ID_INPUT_TABLET"));

    /* accelerometer via prop bit 6 */
    iid_parse_mask("9", ev); iid_parse_mask("0", key);
    iid_parse_mask("0", rel); iid_parse_mask("7", ab); iid_parse_mask("40", pr); /* prop bit6 */
    out.n = 0;
    assert(iid_test_pointers(ev, ab, key, rel, pr, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_ACCELEROMETER"));

    /* touchpad: ABS_X/Y + BTN_TOOL_FINGER, no pen, not direct */
    iid_parse_mask("9", ev);                                  /* SYN|ABS */
    { unsigned long k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_TOOL_FINGER/64] |= 1UL << (BTN_TOOL_FINGER%64);
      /* re-encode into a string is unnecessary; test via direct array */
      iid_parse_mask("3", ab);                                /* ABS_X|ABS_Y */
      iid_parse_mask("0", rel); iid_parse_mask("0", pr);
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_TOUCHPAD"));
      assert(!iid_has(&out, "ID_INPUT_MOUSE")); }

    /* touchscreen: MT coords + INPUT_PROP_DIRECT */
    { unsigned long a[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) a[i]=0;
      a[ABS_MT_POSITION_X/64] |= 1UL << (ABS_MT_POSITION_X%64);
      a[ABS_MT_POSITION_Y/64] |= 1UL << (ABS_MT_POSITION_Y%64);
      iid_parse_mask("9", ev); iid_parse_mask("0", key);
      iid_parse_mask("0", rel); iid_parse_mask("2", pr);      /* prop bit1 = DIRECT */
      out.n = 0;
      assert(iid_test_pointers(ev, a, key, rel, pr, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_TOUCHSCREEN")); }

    /* joystick: BTN_JOYSTICK button */
    { unsigned long k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_JOYSTICK/64] |= 1UL << (BTN_JOYSTICK%64);
      iid_parse_mask("3", ev); iid_parse_mask("0", rel);
      iid_parse_mask("0", ab); iid_parse_mask("0", pr);
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_JOYSTICK")); }

    /* pointingstick: prop bit5 + mouse buttons */
    { unsigned long k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_MOUSE/64] |= 1UL << (BTN_MOUSE%64);
      iid_parse_mask("17", ev); iid_parse_mask("3", rel);
      iid_parse_mask("0", ab); iid_parse_mask("20", pr);      /* prop bit5 */
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_POINTINGSTICK") && iid_has(&out, "ID_INPUT_MOUSE")); }

    printf("test_heuristic OK\n");
}
```

Add `test_heuristic();` to `main`.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `iid_test_key` / `iid_test_pointers` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `input_id.h` before the `#endif`:

```c
static inline int iid_test_key(const unsigned long ev[IID_NWORDS],
                               const unsigned long key[IID_NWORDS], struct uevent *out) {
    if (!iid_test_bit(ev, EV_KEY)) return 0;
    unsigned long found = 0;
    for (int i = 0; i < BTN_MISC / 64; i++) found |= key[i];   /* KEY_* below BTN_* */
    int ret = 0;
    if ((key[0] & 0xFFFFFFFEUL) == 0xFFFFFFFEUL) { iid_emit(out, "ID_INPUT_KEYBOARD", "1"); ret = 1; }
    if (found != 0) { iid_emit(out, "ID_INPUT_KEY", "1"); ret = 1; }
    return ret;
}

static inline int iid_test_pointers(const unsigned long ev[IID_NWORDS], const unsigned long abs_[IID_NWORDS],
                                    const unsigned long key[IID_NWORDS], const unsigned long rel[IID_NWORDS],
                                    const unsigned long prop[IID_NWORDS], struct uevent *out) {
    int has_keys = iid_test_bit(ev, EV_KEY);
    int has_abs = iid_test_bit(abs_, ABS_X) && iid_test_bit(abs_, ABS_Y);
    int has_3d  = has_abs && iid_test_bit(abs_, ABS_Z);
    int is_accel = iid_test_bit(prop, INPUT_PROP_ACCELEROMETER);
    if (!has_keys && has_3d) is_accel = 1;
    if (is_accel) { iid_emit(out, "ID_INPUT_ACCELEROMETER", "1"); return 1; }

    int is_pointing_stick = iid_test_bit(prop, INPUT_PROP_POINTING_STICK);
    int has_stylus = iid_test_bit(key, BTN_STYLUS);
    int has_pen    = iid_test_bit(key, BTN_TOOL_PEN);
    int finger_but_no_pen = iid_test_bit(key, BTN_TOOL_FINGER) && !has_pen;
    int has_mouse_button = iid_any_bit(key, BTN_MOUSE, BTN_JOYSTICK);
    int has_rel = iid_test_bit(ev, EV_REL) && iid_test_bit(rel, REL_X) && iid_test_bit(rel, REL_Y);
    int has_mt  = iid_test_bit(abs_, ABS_MT_POSITION_X) && iid_test_bit(abs_, ABS_MT_POSITION_Y);
    int is_direct = iid_test_bit(prop, INPUT_PROP_DIRECT);
    int has_touch = iid_test_bit(key, BTN_TOUCH);
    int has_joy = iid_any_bit(key, BTN_JOYSTICK, BTN_DIGI)
               || iid_any_bit(key, BTN_TRIGGER_HAPPY, BTN_TRIGGER_HAPPY40 + 1)
               || iid_any_bit(abs_, ABS_RX, ABS_PRESSURE);

    int is_tablet = 0, is_touchpad = 0, is_touchscreen = 0, is_joystick = 0, is_mouse = 0;
    if (has_abs) {
        if (has_stylus || has_pen) is_tablet = 1;
        else if (finger_but_no_pen && !is_direct) is_touchpad = 1;
        else if (has_mouse_button) is_mouse = 1;
        else if (has_touch || is_direct) is_touchscreen = 1;
        else if (has_joy) is_joystick = 1;
    } else if (has_joy) {
        is_joystick = 1;
    }

    if (has_mt) {
        if (is_direct) is_touchscreen = 1;
        else is_touchpad = 1;
    }

    if (!is_tablet && !is_touchpad && !is_joystick && has_mouse_button && (has_rel || !has_abs))
        is_mouse = 1;

    if (is_pointing_stick) iid_emit(out, "ID_INPUT_POINTINGSTICK", "1");
    if (is_mouse)          iid_emit(out, "ID_INPUT_MOUSE", "1");
    if (is_touchpad)       iid_emit(out, "ID_INPUT_TOUCHPAD", "1");
    if (is_touchscreen)    iid_emit(out, "ID_INPUT_TOUCHSCREEN", "1");
    if (is_joystick)       iid_emit(out, "ID_INPUT_JOYSTICK", "1");
    if (is_tablet)         iid_emit(out, "ID_INPUT_TABLET", "1");
    return is_tablet || is_mouse || is_touchpad || is_touchscreen || is_joystick || is_pointing_stick;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: PASS — `test_heuristic OK`. No warnings.

- [ ] **Step 5: Commit**

```bash
git add input_id.h tests/test_input_id.c
git commit -m "feat(input_id): test_key + test_pointers classification"
```

---

### Task 4: orchestrator `input_id_build`

**Files:**
- Modify: `input_id.h`
- Test: `tests/test_input_id.c`

**Interfaces:**
- Consumes: `iid_find_input_node`, `iid_read_masks`, `iid_test_pointers`, `iid_test_key`, `iid_emit`, `iid_test_bit`.
- Produces: `int input_id_build(const char *sysroot, const char *devpath, struct uevent *out)` — resolves node, reads masks, runs the dispatch (switch → pointers → key → scrollwheel-only → master `ID_INPUT`), fills `out`. Returns 0 on success, -1 if not an input device.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_input_id.c`, reusing `iid_make_node`/`iid_has`:

```c
static void test_build_full(void) {
    char root[] = "/tmp/iidbuildXXXXXX";
    assert(mkdtemp(root));

    /* keyboard node */
    iid_make_node(root, "/devices/kbd", "120013", "fffffffffffffffe", "0", "0", "0"); /* word0 = keyboard mask */
    char ev0[PATH_MAX]; snprintf(ev0, sizeof ev0, "%s/devices/kbd/event0", root); iid_mkdirs(ev0);
    struct uevent e; assert(input_id_build(root, "/devices/kbd/event0", &e) == 0);
    assert(iid_has(&e, "ID_INPUT"));
    assert(iid_has(&e, "ID_INPUT_KEYBOARD") && iid_has(&e, "ID_INPUT_KEY"));
    assert(!iid_has(&e, "ID_INPUT_MOUSE"));

    /* switch-only node (audio jack): ev = SYN|SW, sw set */
    iid_make_node(root, "/devices/sw", "21", "0", "0", "0", "0");
    assert(input_id_build(root, "/devices/sw", &e) == 0);
    assert(iid_has(&e, "ID_INPUT") && iid_has(&e, "ID_INPUT_SWITCH"));
    assert(!iid_has(&e, "ID_INPUT_KEY"));

    /* scrollwheel-only: EV_REL + REL_WHEEL, no buttons -> ID_INPUT_KEY + ID_INPUT */
    iid_make_node(root, "/devices/whl", "4", "0", "100", "0", "0");   /* ev = EV_REL only; rel bit8 = REL_WHEEL */
    assert(input_id_build(root, "/devices/whl", &e) == 0);
    assert(iid_has(&e, "ID_INPUT") && iid_has(&e, "ID_INPUT_KEY"));

    /* not an input device */
    assert(input_id_build(root, "/devices", &e) != 0);

    printf("test_build_full OK\n");
}
```

Add `test_build_full();` to `main`.

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_input_id.c -o /tmp/t && /tmp/t`
Expected: FAIL — `input_id_build` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `input_id.h` before the `#endif`:

```c
static inline int input_id_build(const char *sysroot, const char *devpath, struct uevent *out) {
    out->n = 0;
    char inp[PATH_MAX];
    if (iid_find_input_node(sysroot, devpath, inp, sizeof inp) != 0) return -1;

    unsigned long ev[IID_NWORDS], key[IID_NWORDS], rel[IID_NWORDS], abs_[IID_NWORDS], prop[IID_NWORDS];
    iid_read_masks(inp, ev, key, rel, abs_, prop);

    if (iid_test_bit(ev, EV_SW)) iid_emit(out, "ID_INPUT_SWITCH", "1");

    int is_pointer = iid_test_pointers(ev, abs_, key, rel, prop, out);
    int is_key = iid_test_key(ev, key, out);

    if (!is_pointer && !is_key && iid_test_bit(ev, EV_REL)
        && (iid_test_bit(rel, REL_WHEEL) || iid_test_bit(rel, REL_HWHEEL))) {
        iid_emit(out, "ID_INPUT_KEY", "1");
        is_key = 1;
    }

    if (is_pointer || is_key || iid_test_bit(ev, EV_SW))
        iid_emit(out, "ID_INPUT", "1");

    return 0;
}
```

- [ ] **Step 4: Run the full suite**

Run: `make test`
Expected: PASS — every existing suite plus `test_mask` / `test_discovery` / `test_heuristic` / `test_build_full` / `ALL input_id tests passed`. No warnings.

- [ ] **Step 5: Verify the boundary**

Run: `git diff origin/master -- schema-udev.c schema-udev.h; grep -c input_id schema-udev.c`
Expected: empty diff, `0`.

- [ ] **Step 6: Commit**

```bash
git add input_id.h tests/test_input_id.c
git commit -m "feat(input_id): orchestrator input_id_build + dispatch"
```

---

### Task 5: live parity harness + vmtest

**Files:**
- Create: `tests/verify_input_id_live.sh`

**Interfaces:**
- Consumes: `input_id_build` from `input_id.h`.
- Produces: an executable acceptance script; prints `input_id live parity: N devices, M mismatches` and exits non-zero if M > 0.

- [ ] **Step 1: Write the harness**

Create `tests/verify_input_id_live.sh` (make it executable, `chmod +x`):

```sh
#!/bin/sh
# Live parity gate: run input_id_build over every /sys device real udev gave
# ID_INPUT=1, diff emitted keys vs `udevadm info` BOTH directions.
# Read-only. Requires a systemd-udev-populated box (blakbox).
set -eu
cd "$(dirname "$0")/.."

cat > /tmp/inputid_driver.c <<'EOF'
#include "input_id.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct uevent ev;
    if (input_id_build("/sys", argv[1], &ev) != 0) return 0;
    for (int i = 0; i < ev.n; i++) printf("%s=%s\n", ev.key[i], ev.val[i]);
    return 0;
}
EOF
gcc -O2 -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/inputid_driver.c -o /tmp/inputid_driver

props=$(mktemp)
misses=$(mktemp)
total=0
for dev in $(find /sys/devices -name uevent -printf '%h\n'); do
    devpath=${dev#/sys}
    udevadm info -q property -p "$dev" 2>/dev/null > "$props"
    grep -qx 'ID_INPUT=1' "$props" || continue
    total=$((total + 1))
    emitted=$(/tmp/inputid_driver "$devpath")
    # forward: every key we emit must be present verbatim in udev's set
    printf '%s\n' "$emitted" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        grep -qxF "$line" "$props" || printf 'MISMATCH(val) %s | emit=%s\n' "$devpath" "$line"
    done >> "$misses"
    # reverse: every ID_INPUT* udev has, we must emit (under-emission)
    grep -oE '^ID_INPUT[A-Z_]*=1' "$props" | while IFS= read -r uline; do
        printf '%s\n' "$emitted" | grep -qxF "$uline" \
            || printf 'MISMATCH(miss) %s | udev=%s\n' "$devpath" "$uline"
    done >> "$misses"
done

miss=$(grep -c '^MISMATCH' "$misses" || true)
cat "$misses"
printf 'input_id live parity: %d devices, %d mismatches\n' "$total" "$miss"
rm -f "$props" "$misses"
[ "$miss" -eq 0 ]
```

- [ ] **Step 2: Run the live gate**

Run: `chmod +x tests/verify_input_id_live.sh && tests/verify_input_id_live.sh`
Expected: `input_id live parity: 41 devices, 0 mismatches` and exit 0.

If any `MISMATCH` prints: the emitted line and udev's value are shown. Fix the heuristic/parse in `input_id.h` (consult the systemd source), re-run. Do NOT touch `schema-udev.c`.

- [ ] **Step 3: Run the vmtest boot rail**

Run: `cd ~/schema-livetest && ./vmtest.sh` then `cd -`
Expected: `>> RESULT: PASS`. Confirms the new header does not disturb the PID-1 boot.

- [ ] **Step 4: Commit**

```bash
git add tests/verify_input_id_live.sh
git commit -m "test(input_id): live udev parity acceptance harness (both directions)"
```

- [ ] **Step 5: Push and confirm origin == local (hard rule)**

```bash
git push -u origin feat/schema-udev-input-id
git ls-remote origin refs/heads/feat/schema-udev-input-id
git rev-parse HEAD
```

Expected: the two hashes are identical. Only then is the work landable.

---

## Notes for the implementer (Greg)

- The **crux** is the reversed-word bitmask parse (Task 1). Get that exactly right — every downstream test depends on it. Ground-truth samples: keyboard `key` word0 = `fffffffffffffffe`; mouse `key = 1f0000 0 0 0 0` (BTN_LEFT.. in word4); switch `ev = 21` (`SYN|SW`), `sw = 140`.
- `properties` is a **sibling** of `capabilities/` — not `capabilities/prop`. Reading the wrong path passes the blakbox gate (all-zero here) but breaks accelerometer/touchpad/touchscreen on other machines.
- `schema-udev.c` and `.h` are frozen. If a test seems to need a change there, stop — it doesn't.
- The live gate is the final authority. If the systemd source and this plan's heuristic ever differ, follow the source and let the gate confirm.
- After I (Claire) verify all gates, the branch lands via PR. Do not open the PR yourself.
