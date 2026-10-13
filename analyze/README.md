# schema-analyze

Boot timing across reboots. `schema-ctl analyze` shows one boot's critical chain;
PID 1 forgets it on reboot. `schema-analyze` keeps a file per boot and compares them.

schema-init's first Rust component: std only, no crates, off the boot path
(it only reads PID 1's control socket after boot). The RPM ships it; recording
is opt-in per box, like every reference service:

```
sudo cp /usr/share/schema-init/services/schema-analyze.svc.example \
        /etc/schema-init/services/schema-analyze.svc     # record each boot at +120s
sudo schema-ctl reload
```

From source, without the RPM (a plain `make` needs no Rust toolchain):

```
make analyze                # as your user
sudo make install-analyze   # installs the already-built binary; never runs cargo
```

| command | |
|---|---|
| `record [--force]` | save this boot's `timing` + `analyze` from PID 1 to `/var/lib/schema-init/boots/` (root); one file per kernel boot_id, `--force` replaces it |
| `history [-n N]` | one line per boot: kernel→PID 1, userspace, total, PID 1 RSS, slowest service |
| `compare [A [B]]` | per-service cost change A→B, biggest moves first (default previous→latest; `--all`) |
| `regress [--window N] [--min S]` | latest boot vs the median of up to N earlier boots on the same host; exit 1 when a service is slower by more than S seconds *and* 25% |
| `import FILE...` | backfill from `tests/livetest/boot-capture.sh` reports |

Boots are named `<booted-at>-<host>`; select one by `#index` from `history`,
`-1` for the latest, or an id prefix such as `20261011`. Given no boot,
`compare` takes the latest; given one boot, it compares that boot with the
previous boot from the same host. Reports from before
`timing` had a cost column carry only the readiness instant; rows compared on
it are marked `(ready@)`. `--dir DIR` or `SCHEMA_ANALYZE_DIR` points at another store.

```
cargo test --manifest-path analyze/Cargo.toml
```
