#!/usr/bin/env python3
# Deterministic proof of the boot-success-guard GRUB decision logic.
#
# schema-vmtest direct-boots the kernel (rdinit=/sbin/schema-init) and never
# runs GRUB, so it structurally cannot exercise the boot-counter fallback. This
# test is the honest substitute: it runs the REAL emitted GRUB blocks
# (stock 08_fallback_counting + our scripts/09_schema_fallback) through a
# faithful model of GRUB's shell across the full state matrix, and asserts the
# selected `default`.
#
# Load-bearing safety properties proven here:
#   1. An UNARMED boot (no boot_counter) never selects the fallback.
#   2. A boot that SUCCEEDED (boot_success=1) never selects the fallback.
#   3. When armed and the counter is exhausted, the fallback IS selected.
#   4. A missing schema_fallback_id degrades to stock `default=1`, never a brick.

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOK09 = os.path.join(REPO, "scripts", "09_schema_fallback")

# Stock 08_fallback_counting (mirrors Fedora/GRUB). Prefer the live copy on
# this box so the test tracks exactly what will boot; fall back to this literal
# for portability / CI. Kept in sync with the shipped 08.
STOCK_08 = """insmod increment
if [ -n "${boot_counter}" -a "${boot_success}" = "0" ]; then
  if  [ "${boot_counter}" = "0" -o "${boot_counter}" = "-1" ]; then
    set default=1
    set boot_counter=-1
  else
    decrement boot_counter
  fi
  if [ "${env_block}" ]; then
    save_env -f "${env_block}" boot_counter
  else
    save_env boot_counter
  fi
fi
"""


def block_08():
    live = "/etc/grub.d/08_fallback_counting"
    if os.path.exists(live):
        # The grub.d scripts emit the grub block on stdout.
        return subprocess.run(["sh", live], capture_output=True, text=True,
                              check=True).stdout
    return STOCK_08


def block_09():
    return subprocess.run(["sh", HOOK09], capture_output=True, text=True,
                          check=True).stdout


def grubify(block):
    """Rewrite GRUB builtins to shell functions so the real block text runs
    verbatim under /bin/sh (var refs, if/test, quoting all preserved)."""
    out = []
    for line in block.splitlines():
        stripped = line.lstrip()
        indent = line[: len(line) - len(stripped)]
        if stripped.startswith("insmod ") or stripped.startswith("save_env "):
            out.append(indent + ":")
        elif stripped.startswith("set "):
            out.append(indent + "grub_set " + stripped[4:])
        elif stripped.startswith("decrement "):
            out.append(indent + "grub_decrement " + stripped[10:])
        else:
            out.append(line)
    return "\n".join(out)


PRELUDE = r"""
grub_set() { eval "$1"; }
grub_decrement() { eval "$1=\$(( \$$1 - 1 ))"; }
env_block="512+1"
default="SAVEDENTRY"
"""


def run_boot(boot_counter, boot_success, fallback_id, b08, b09):
    setup = ""
    if boot_counter is not None:
        setup += f'boot_counter="{boot_counter}"\n'
    setup += f'boot_success="{boot_success}"\n'
    if fallback_id is not None:
        setup += f'schema_fallback_id="{fallback_id}"\n'
    script = PRELUDE + setup + grubify(b08) + "\n" + grubify(b09) + '\necho "$default"\n'
    r = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"grub model errored: {r.stderr}")
    return r.stdout.strip()


# (boot_counter, boot_success, fallback_id, expected_default, note)
CASES = [
    (None, "1", "FBID", "SAVEDENTRY", "unarmed + success -> normal"),
    (None, "0", "FBID", "SAVEDENTRY", "UNARMED failed boot -> normal (never fallback)"),
    ("2", "1", "FBID", "SAVEDENTRY", "armed but this boot succeeded -> normal"),
    ("3", "0", "FBID", "SAVEDENTRY", "armed, failed, counter high -> decrement only"),
    ("2", "0", "FBID", "SAVEDENTRY", "armed N=2 first failed boot -> tolerate"),
    ("1", "0", "FBID", "FBID", "08 decrements 1->0, 09 fires fallback (one boot before stock default=1)"),
    ("0", "0", "FBID", "FBID", "already 0 -> fallback"),
    ("-1", "0", "FBID", "FBID", "pinned -1 -> fallback"),
    ("1", "0", None, "SAVEDENTRY", "1->0 with no fallback id: 09 no-op, stock hasn't hit default=1 yet -> normal"),
    ("0", "0", None, "1", "0 with no fallback id -> stock default=1"),
    ("2", "1", None, "SAVEDENTRY", "success, no id -> normal"),
    (None, "0", None, "SAVEDENTRY", "unarmed, no id -> normal"),
]


def main():
    b08, b09 = block_08(), block_09()
    fails = 0
    for bc, bs, fid, expected, note in CASES:
        got = run_boot(bc, bs, fid, b08, b09)
        ok = got == expected
        fails += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] counter={bc!s:>5} success={bs} "
              f"id={'set' if fid else 'unset'} -> default={got!r} "
              f"(want {expected!r})  # {note}")
    if fails:
        print(f"\nBOOTGUARD LOGIC: {fails} FAILED", file=sys.stderr)
        sys.exit(1)
    print(f"\nBOOTGUARD LOGIC: all {len(CASES)} cases pass")


if __name__ == "__main__":
    main()
