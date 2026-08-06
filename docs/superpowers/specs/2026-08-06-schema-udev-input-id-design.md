# schema-udev builtin #3: input_id — Design

**Status:** approved 2026-08-06
**Builtin:** #3 of the udevd-retirement cutover worklist (Phase 3b), after path_id (#1) and usb_id (#2).
**Boundary:** mechanism-only, off by default, `schema-udev.c` byte-identical. Wired to nothing live.

## Goal

Reproduce, byte-for-byte, the `ID_INPUT` / `ID_INPUT_*` properties that systemd-udevd's
`input_id` builtin synthesizes for Linux input devices — by reading the device's sysfs
capability bitmasks and running udev's fixed classification heuristic. Acceptance is
**0 mismatches vs real udev across the 41 input devices on blakbox**, with the full
algorithm implemented (all device classes) so the builtin is correct on hardware this box
does not currently have.

## Normative reference

This is a faithful port of systemd `src/udev/udev-builtin-input_id.c` (the `test_pointers`,
`test_key`, and main-dispatch logic). Where this document and the systemd source disagree,
**the systemd source governs and the live parity gate is the final authority.** Greg should
consult the upstream file directly while implementing; this spec pins the algorithm, the exact
sysfs layout, and the bitmask encoding so the port is unambiguous.

## Ground truth (blakbox, decoded from `/run/udev/data` + `/sys`)

Flags that actually occur here and their counts:

| Property | Count | Meaning |
|---|---|---|
| `ID_INPUT` | 41 | master flag — device is an input device |
| `ID_INPUT_SWITCH` | 18 | `EV_SW` present |
| `ID_INPUT_KEY` | 18 | has some `KEY_*` (below `BTN_MISC`) but not a full keyboard |
| `ID_INPUT_KEYBOARD` | 6 | full keyboard |
| `ID_INPUT_MOUSE` | 3 | mouse |

No touchpad / touchscreen / joystick / tablet / accelerometer / pointingstick hardware is
attached here — those branches are implemented and unit-tested against fabricated bitmasks,
not exercised by the live gate.

**Only `=1` lines are ever emitted.** udev never writes `ID_INPUT_MOUSE=0`. Absence == false.

## Sysfs layout (exact — the gate cannot catch a wrong path here)

The masks live on the **input device** node (`.../input/inputN`), NOT the `eventN` child. Two of
them sit in different places:

- `<inputN>/capabilities/ev`
- `<inputN>/capabilities/key`
- `<inputN>/capabilities/rel`
- `<inputN>/capabilities/abs`
- `<inputN>/properties`  ← **sibling of `capabilities/`, not `capabilities/prop`.** This is the
  `INPUT_PROP_*` bitmask. On blakbox every device reads `0`, so a wrong path would still pass the
  live gate while silently breaking accelerometer / pointingstick / touchpad-vs-touchscreen
  detection on other machines. It must be read from `<inputN>/properties`.

### Node resolution

The builtin is invoked with a devpath that may be the `eventN` node, a `jsN` node, or the
`inputN` node itself. Walk up the parent chain to the **nearest ancestor directory that
contains `capabilities/ev`** (this is the `inputN` node). Reuse path_id.h's `pi_parent` /
`pi_base` for the walk (DRY). If none is found, emit nothing and return non-zero (not an input
device).

## Bitmask encoding (the crux)

`cat capabilities/key` prints space-separated hex longs, **most-significant word first, word0
rightmost.** Example (mouse): `key = 1f0000 0 0 0 0` → 5 words, word4=`0x1f0000`, word0=`0`.
`0x1f0000` in word4 = bits 256+16 .. 256+20 = `BTN_LEFT..BTN_EXTRA`.

Parse each field into a **fixed-size, zero-filled** `unsigned long[]` (word0 = last token). A
field may print fewer words than the array holds; missing high words are zero. Sizing: key needs
`KEY_MAX+1 = 0x300 = 768` bits (12 × 64-bit words); ev/rel/abs/prop are far smaller but use the
same helper. `BITS_PER_LONG` = 64 on this target (the harness/tests compile `-std=c99` on
x86-64; assume 64-bit longs, matching the live kernel's print format).

```
test_bit(arr, n)  ==  (arr[n / 64] >> (n % 64)) & 1
```

Verified against ground truth:
- Keyboard word0 = `fffffffffffffffe` ⊇ `0xFFFFFFFE` (bits 1–31 all set) → keyboard. ✔
- KEY-only device word0 = `8000` / `c000` / `100000`(word3) — bits 1–31 not all set → no
  keyboard, but OR of words 0–3 > 0 → `ID_INPUT_KEY`. ✔

## Algorithm (port)

Constants from `linux/input-event-codes.h`: `EV_SYN=0 EV_KEY=1 EV_REL=2 EV_ABS=3 EV_MSC=4
EV_SW=5`; `ABS_X=0 ABS_Y=1 ABS_Z=2 ABS_MT_SLOT=0x2f ABS_MT_POSITION_X=0x35
ABS_MT_POSITION_Y=0x36 ABS_RX=0x03 ABS_PRESSURE=0x18`; `REL_X=0 REL_Y=1 REL_WHEEL=8
REL_HWHEEL=6`; `BTN_MISC=0x100 BTN_MOUSE=0x110 BTN_JOYSTICK=0x120 BTN_DIGI=0x140
BTN_TOOL_PEN=0x140 BTN_TOOL_FINGER=0x145 BTN_TOUCH=0x14a BTN_STYLUS=0x14b
BTN_TRIGGER_HAPPY=0x2c0 BTN_TRIGGER_HAPPY40=0x2e7`; `INPUT_PROP_DIRECT=1
INPUT_PROP_POINTING_STICK=5 INPUT_PROP_ACCELEROMETER=6`.

**`test_pointers(ev, abs, key, rel, props)`** → sets at most the pointer-family flags, returns
whether any matched:
1. `has_keys = test_bit(EV_KEY, ev)`; `has_abs = test_bit(ABS_X,abs) && test_bit(ABS_Y,abs)`;
   `has_3d = has_abs && test_bit(ABS_Z,abs)`.
2. `is_accelerometer = test_bit(INPUT_PROP_ACCELEROMETER, props)`; if `!has_keys && has_3d` →
   `is_accelerometer = true`. If `is_accelerometer` → emit `ID_INPUT_ACCELEROMETER`, return true.
3. `is_pointing_stick = test_bit(INPUT_PROP_POINTING_STICK, props)`.
   `has_stylus = test_bit(BTN_STYLUS,key)`; `has_pen = test_bit(BTN_TOOL_PEN,key)`;
   `finger_but_no_pen = test_bit(BTN_TOOL_FINGER,key) && !has_pen`.
4. `has_mouse_button` = any bit in `[BTN_MOUSE, BTN_JOYSTICK)` of key.
5. `has_rel = test_bit(EV_REL,ev) && test_bit(REL_X,rel) && test_bit(REL_Y,rel)`.
6. `has_mt = test_bit(ABS_MT_POSITION_X,abs) && test_bit(ABS_MT_POSITION_Y,abs)`.
7. `is_direct = test_bit(INPUT_PROP_DIRECT, props)`; `has_touch = test_bit(BTN_TOUCH,key)`.
8. `has_joystick_axes_or_buttons`: any bit in `[BTN_JOYSTICK, BTN_DIGI)`, or in
   `[BTN_TRIGGER_HAPPY, BTN_TRIGGER_HAPPY40]`, or in abs `[ABS_RX, ABS_PRESSURE)`.
9. If `has_abs`: stylus||pen → tablet; else finger_but_no_pen && !is_direct → touchpad; else
   has_mouse_button → mouse; else has_touch||is_direct → touchscreen; else
   has_joystick_axes_or_buttons → joystick. Else if has_joystick_axes_or_buttons → joystick.
10. If `has_mt`: is_direct → touchscreen else touchpad.
11. If `!is_tablet && !is_touchpad && !is_joystick && has_mouse_button && (has_rel || !has_abs)`
    → mouse.
12. Emit (each if true, in this order): `ID_INPUT_POINTINGSTICK`, `ID_INPUT_MOUSE`,
    `ID_INPUT_TOUCHPAD`, `ID_INPUT_TOUCHSCREEN`, `ID_INPUT_JOYSTICK`, `ID_INPUT_TABLET`.
    Return `is_tablet||is_mouse||is_touchpad||is_touchscreen||is_joystick||is_pointing_stick`.

**`test_key(ev, key)`** → returns whether any key flag matched:
1. If `!test_bit(EV_KEY, ev)` → return false.
2. `found = OR of key[i] for i in [0, BTN_MISC/64) = i in {0,1,2,3}` (all `KEY_*` below the
   `BTN_*` range).
3. If `(key[0] & 0xFFFFFFFE) == 0xFFFFFFFE` → emit `ID_INPUT_KEYBOARD`.
4. If `found != 0` → emit `ID_INPUT_KEY`.
5. Return true if either emitted.

**Main dispatch (`input_id_build`)**, in this order:
1. Resolve `inputN`, read the five masks.
2. If `test_bit(EV_SW, ev)` → emit `ID_INPUT_SWITCH`.
3. `is_pointer = test_pointers(...)`; `is_key = test_key(...)`.
4. Scrollwheel-only fallback: if `!is_pointer && !is_key && test_bit(EV_REL,ev) &&
   (test_bit(REL_WHEEL,rel) || test_bit(REL_HWHEEL,rel))` → emit `ID_INPUT_KEY` (set is_key).
5. If `is_pointer || is_key || test_bit(EV_SW,ev)` → emit `ID_INPUT=1`.
   (Accelerometer counts as pointer via test_pointers' return.)

## Components

- **`input_id.h`** (new): includes `path_id.h` (→ `schema-udev.h`) for sysfs-walk primitives.
  Public: `int input_id_build(const char *sysroot, const char *devpath, struct uevent *out)`.
  Internal: `iid_parse_mask` (reversed-word hex → zero-filled array + returns word count),
  `iid_test_bit`, `iid_find_input_node`, `iid_test_pointers`, `iid_test_key`. Emits via the same
  `UEMIT`-style guard used by usb_id, capped at `UE_MAX_KEYS`.
- **`tests/test_input_id.c`** (new): `iid_parse_mask`/`iid_test_bit` edge cases (short fields,
  high words, word0 keyboard mask); one fabricated `capabilities/`+`properties` tree per class
  (keyboard, key-only, mouse, switch, touchpad, touchscreen, joystick, tablet, accelerometer,
  pointingstick, scrollwheel-only) asserting the exact emitted key set.
- **`tests/verify_input_id_live.sh`** (new): for every `/sys` device with `ID_INPUT=1` in udev's
  view, run `input_id_build`, exact full-line diff of emitted keys vs `udevadm info`. Expect
  **41 devices, 0 mismatches.** Both-directions (also flag any `ID_INPUT*` udev has that we omit).
- **`Makefile`**: one line to build+run `tests/test_input_id.c`.
- **`schema-udev.c` / `schema-udev.h`**: unchanged (byte-identical; fits existing `UE_MAX_KEYS`).

## Testing / acceptance gate

1. `make test` green incl. `test_input_id`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c` empty; `grep input_id schema-udev.c` empty.
3. Live: `tests/verify_input_id_live.sh` → **41 devices, 0 mismatches**, both directions.
4. `vmtest.sh` → RESULT: PASS (new header must not disturb the PID-1 boot rail).

## Out of scope

`ID_INPUT_TABLET_PAD` (needs an extra BTN scan udev gates behind tablet detection — no such
hardware here; add only if a future device requires it). hwdb-derived names. Any live wiring.
