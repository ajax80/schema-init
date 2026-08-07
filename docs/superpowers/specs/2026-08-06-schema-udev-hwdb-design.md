# schema-udev builtin #6: hwdb — Design

**Status:** approved 2026-08-06
**Builtin:** #6 of the udevd-retirement cutover (Phase 3b) — the **last** builtin, after path_id (#1), usb_id (#2), input_id (#3), net_id (#4), blkid PR-A/PR-B (#5).
**Boundary:** mechanism-only, off by default, `schema-udev.c` / `schema-udev.h` byte-identical. Wired to nothing live.

## Goal

Reproduce, byte-for-byte, the `*_FROM_DATABASE` properties that systemd-udevd's `hwdb` builtin
emits — by reading systemd's **compiled binary trie** `hwdb.bin` and glob-matching a device's
`modalias`. Acceptance is **0 mismatches vs real udev across the ~199 modalias devices on blakbox**
(both directions) on the `*_FROM_DATABASE` subset (excluding the composite `ID_OUI_FROM_DATABASE`).

## Architecturally different from the other five

The other builtins decode device state; hwdb **queries a database**. The single hard part is the
binary-trie reader + glob matcher; once it works, every lookup is just a different key string. The
only per-device work is key construction, and for all `*_FROM_DATABASE` output the key is simply the
`modalias` sysattr. Composite-key lookups (evdev/mouse/keyboard/sensor/net:naming/OUI) need the
rules engine's multi-attribute key construction and are **deferred** (the trie reader still serves
them via `hwdb_query` with a literal key).

## Normative reference & validation

Faithful port of systemd v259 `src/libsystemd/sd-hwdb/{hwdb-internal.h,sd-hwdb.c}` (`trie_search_f`,
`trie_fnmatch_f`, `hwdb_add_property`, `node_lookup_f`). **Validated:** a from-scratch reimpl of this
algorithm run against blakbox's real `/etc/udev/hwdb.bin` returned exactly udev's 4 properties for
the AMD root-complex modalias (`ID_VENDOR/MODEL/PCI_CLASS/PCI_SUBCLASS_FROM_DATABASE`). Source
governs; the live gate is the authority.

## Binary format (verified against blakbox `hwdb.bin`, 14 MB)

`hwdb.bin` is `-r--r--r--` (world-readable) → **no sudo**. Located at `/etc/udev/hwdb.bin`, fallback
`/usr/lib/udev/hwdb.bin`. All multi-byte fields little-endian on disk.

**Header** (`struct trie_header_f`, offset 0):
| off | field | blakbox value |
|---|---|---|
| 0 | `signature[8]` | `KSLPHHRH` |
| 8 | `tool_version` u64 | 259 |
| 16 | `file_size` u64 | 14367040 |
| 24 | `header_size` u64 | (= 80) |
| 32 | `node_size` u64 | **24** |
| 40 | `child_entry_size` u64 | **16** |
| 48 | `value_entry_size` u64 | **32** (≥ sizeof v2 ⇒ V2 entries) |
| 56 | `nodes_root_off` u64 | 11481432 |
| 64 | `nodes_len` u64 | — |
| 72 | `strings_len` u64 | — |

**Node** (`struct trie_node_f`, at an absolute offset into the map):
`prefix_off` u64 (→ strings), `children_count` u8, `padding[7]`, `values_count` u64. Followed by
`children_count` child entries (at `node + node_size`), then `values_count` value entries.

**Child** (`struct trie_child_entry_f`, `child_entry_size`=16): `c` u8, `padding[7]`, `child_off`
u64. Children are sorted by `c` (binary-searchable).

**Value** (V2 `struct trie_value_entry2_f`, `value_entry_size`=32): `key_off` u64, `value_off` u64,
`filename_off` u64, `line_number` u32, `file_priority` u16, `padding` u16. (V1 = first 16 bytes
only; select by `value_entry_size >= 32`.) Only `key_off`/`value_off` (+ priority/line for dedup)
are used.

**String:** `trie_string(off)` = NUL-terminated string at `map + off`.

## Search algorithm (port of `trie_search_f` + `trie_fnmatch_f`)

`hw_search(h, modalias, collector)`:
1. Start at `nodes_root_off`, `i = 0` into the search string.
2. Match the node's prefix char-by-char: on a `*`/`?`/`[` prefix char → `hw_fnmatch(node, p, …)`
   and return; on a mismatch → return (dead end).
3. After the prefix, for each glob child `*`, `?`, `[` present → push the char to the linebuf and
   recurse `hw_fnmatch`.
4. If the search string is exhausted (`search[i] == '\0'`) → add all this node's values, return.
5. Else look up the child for byte `search[i]`; descend (`i++`). No child → return.

`hw_fnmatch(h, node, p, linebuf, search, collector)`: append `prefix+p` to the linebuf; for each
child, push its byte and recurse; if the node has values and `fnmatch(linebuf, search, 0) == 0`,
add all its values; pop back.

**`hw_add_value` (port of `hwdb_add_property`):** read the value's `key_off`; **the key must start
with `' '`** (else internal → skip); skip that space. Read `value_off`. Insert into the collector
with dedup: on a repeated property name, keep the incumbent unless the new entry has strictly higher
`file_priority`, or equal priority and `line_number >= ` incumbent's (higher/newer line wins).

`fnmatch()` is libc `<fnmatch.h>` — no glob reimplementation.

## Emission

- Lookup key = the device's `modalias` sysattr (read via `pi_sysattr`). No modalias → emit nothing.
- Collect matches with the priority-dedup above, then emit each `key=value` into `out`
  (cap `UE_MAX_KEYS`). Values are emitted verbatim (vendor strings contain spaces/commas/brackets,
  e.g. `Advanced Micro Devices, Inc. [AMD]`) — no encoding.

## Components

- **`hwdb.h`** (new): includes `path_id.h`. Types: `struct hwdb { const unsigned char *map; size_t
  size; uint64_t node_size, child_entry_size, value_entry_size, root_off; }`; a `struct hw_collect`
  (fixed arrays of key/val/priority/line, `HW_MAX_PROPS`); `struct hw_linebuf` (`HW_LINE_MAX`).
  Functions: `hwdb_open(path,h)` (mmap+validate sig+parse header), `hwdb_close`,
  `hw_le16/32/64`, `hw_string`, node/child/value accessors with bounds checks, `hw_child_lookup`,
  `hw_add_value`, `hw_fnmatch`, `hw_search`, `hwdb_query(h,key,out)`, and
  `hwdb_build(sysroot,devpath,out)` (locate hwdb.bin, read modalias, search, emit).
- **`tests/test_hwdb.c`** (new): a hand-built minimal in-memory trie (the 14 MB file can't be a unit
  fixture) exercising: literal descent → value emit; a `*` glob child → `fnmatch` emit; the
  leading-space key filter (a space-less key is not emitted); priority tiebreak (two values, same
  key, different `file_priority`/`line` → higher wins); a no-match modalias → nothing.
- **`tests/verify_hwdb_live.sh`** (new): for every `/sys` device with a `modalias`, run the driver,
  diff the `*_FROM_DATABASE` subset of `udevadm info` both directions, **excluding
  `ID_OUI_FROM_DATABASE`** (composite OUI lookup — deferred). Expect **0 mismatches** across the
  ~199 devices. No sudo.
- **`Makefile`**: one line to build+run `tests/test_hwdb.c` (link needs nothing beyond libc;
  `fnmatch`/`mmap` are in libc).
- **`schema-udev.c` / `schema-udev.h`**: unchanged (byte-identical).

## Testing / acceptance gate

1. `make test` green incl. `test_hwdb`, `-Wall -Wextra` clean.
2. Boundary: `git diff master -- schema-udev.c schema-udev.h` empty; `grep hwdb schema-udev.c`
   empty (the substring `hwdb` must not appear — the builtin is unwired).
3. Live: `tests/verify_hwdb_live.sh` → **0 mismatches** across the modalias devices, both directions.
4. `vmtest.sh` → RESULT: PASS.

## Out of scope

- Composite-key lookups: evdev/mouse/keyboard/touchpad/sensor/joystick/net:naming/OUI/camera/
  ieee1394 — these need the rules engine's multi-attribute key construction. The trie reader serves
  them via `hwdb_query` (unit-tested with a literal key); the per-lookup key builders are a future
  rules-engine piece.
- `ID_OUI_FROM_DATABASE` (composite `OUI:<mac>` key) — excluded from the live gate.
- Compiling/updating `hwdb.bin` (`systemd-hwdb update`) — we consume systemd's compiled file.
- Any live wiring — `schema-udev.c` stays byte-identical.

## Notable risk

The glob recursion (`hw_fnmatch`) and the exact `trie_search_f` control flow (when to fnmatch vs
descend, the leading-space filter, the priority tiebreak) are the correctness core. The linebuf must
be bounded (`HW_LINE_MAX`, e.g. 2048) and every map access bounds-checked against `size` to stay
memory-safe on a large mmap. The algorithm is validated end-to-end against the real file; the live
gate over ~199 devices is the final authority.
