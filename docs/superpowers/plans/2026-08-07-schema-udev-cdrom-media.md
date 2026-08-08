# schema-udev slice 3e — cdrom_id media + optical filesystem — Implementation Plan

> **For agentic workers:** implement task-by-task. Each task ends with a green test and a commit. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extend the existing `cdrom_id` builtin from drive-capabilities (3d) to also report media status (`ID_CDROM_MEDIA*`) and the optical filesystem (`ID_FS_*` via ISO9660 + UDF), reliably against a not-ready/cold drive.

**Architecture:** All media logic lands in `cdrom_id.h`; optical filesystem probing in a new `optical_fs.h`. `cdrom_id_build` becomes the optical orchestrator: GET CONFIG (caps + current-profile media type) → TEST UNIT READY bounded poll → READ DISC INFO → READ TOC → optical_fs_probe. Steps after GET CONFIG run only once the drive reports media-ready. No new builtin, no `ub_select` change, no `schema-udev.c` change.

**Tech Stack:** C (C11, header-only builtin pattern), Linux SG_IO (`scsi/sg.h`), MMC commands.

## Global Constraints

- **Never edit `schema-udev.c` in this slice.** The netlink bind stays byte-identical: `sa.nl_groups = 1;` (kernel uevents only; NEVER group 2).
- Shadow db path stays `/run/schema-udev/data` (ours), never `/run/udev/data`.
- First-writer-wins on every merge (`CEMIT`/`bpt_emit`/`ub_add`); never overwrite a kernel-provided property.
- **Never fabricate a key.** If a read fails or a field is absent, omit it — do not guess. Emit count keys (`ID_CDROM_MEDIA_TRACK_COUNT_DATA`/`_AUDIO`) only when the count is > 0 (real udev omits zero counts).
- Bounded retries only — no unbounded loop is permitted in the event path.
- Mechanism-only; one-header-per-builtin pattern; header-only (`static inline`).
- E: lines are derived properties only; `V:1` trailing; semantic/set parity (order-independent); byte-parity is a non-goal.
- Parity gate `sudo ./udev-parity` must remain **0/0** both directions; 3a/3b/3c/3d intact.

## Ground truth (real hardware, captured 2026-08-07, sr0 = b11:0)

Two committed fixtures, both from real systemd-udevd `/run/udev/data/b11:0`:

**Burned DVD-R "Wardriver"** (`tests/fixtures/cdrom_media_wardriver.h`) — real udev emits, in scope for us:
```
ID_CDROM_MEDIA=1
ID_CDROM_MEDIA_DVD_R=1
ID_CDROM_MEDIA_SESSION_COUNT=2
ID_CDROM_MEDIA_STATE=appendable
ID_CDROM_MEDIA_TRACK_COUNT=2
ID_CDROM_MEDIA_TRACK_COUNT_DATA=1
ID_FS_TYPE=iso9660
ID_FS_USAGE=filesystem
ID_FS_SYSTEM_ID=LINUX
ID_FS_LABEL=Wardriver.2026.1080p.WEBRip.x264   (+ ID_FS_LABEL_ENC)
ID_FS_UUID=2026-05-09-01-34-23-00              (+ ID_FS_UUID_ENC)
ID_FS_APPLICATION_ID=K3B\x20THE\x20CD\x20KREATOR...
ID_FS_VERSION=Joliet Extension
```
Deferred (real udev emits, we intentionally omit — kept 0/0 via parity deferrals):
`ID_CDROM_MEDIA_SESSION_NEXT` (needs READ TRACK INFORMATION for correct cross-state semantics), `ID_FS_SIZE`, `ID_FS_BLOCKSIZE` (geometry — already in `parity_deferred`).

**Blank DVD-R** (`tests/fixtures/cdrom_media_blank.h`) — real udev emits:
```
ID_CDROM_MEDIA=1
ID_CDROM_MEDIA_DVD_R=1
ID_CDROM_MEDIA_SESSION_COUNT=1
ID_CDROM_MEDIA_STATE=blank
ID_CDROM_MEDIA_TRACK_COUNT=1        (invisible incomplete track — NOT zero)
```
No `ID_CDROM_MEDIA_TRACK_COUNT_DATA` (READ TOC fails 0x05/0x24 on blank → 0 data tracks → omit). No `ID_FS_*` (no filesystem). Deferred: `ID_CDROM_MEDIA_SESSION_NEXT`.

**UDF bridge disc "POWERT_TOUR_DVD"** (`tests/fixtures/cdrom_media_udf.h`) — a DVD-ROM (current profile 0x0010) carrying BOTH a UDF filesystem and an ISO9660 PVD. Real udev reports it as **udf** (not iso9660):
```
ID_CDROM_MEDIA=1  ID_CDROM_MEDIA_DVD=1  ID_CDROM_MEDIA_STATE=complete
ID_CDROM_MEDIA_SESSION_COUNT=1  ID_CDROM_MEDIA_TRACK_COUNT=1  ID_CDROM_MEDIA_TRACK_COUNT_DATA=1
ID_FS_TYPE=udf   ID_FS_USAGE=filesystem   ID_FS_VERSION=1.02
ID_FS_LABEL=POWERT_TOUR_DVD  (+ ID_FS_LABEL_ENC)   ID_FS_LOGICAL_VOLUME_ID=POWERT_TOUR_DVD
ID_FS_VOLUME_ID=POWERT_TOUR_DVD   ID_FS_VOLUME_SET_ID=3655822E
ID_FS_UUID=3655822e00000000  (+ ID_FS_UUID_ENC)
ID_FS_APPLICATION_ID=Apple\x20Computer\x2c\x20Inc.   (encoded form)
```
Verified UDF offsets (confirmed against this disc): AVDP @ LBA 256, tag id `d[0]|d[1]<<8 == 2`; Main VDS location = `d[20..23]` LE, length = `d[16..19]` LE (here LBA 32, 16 sectors). In each VDS sector, tag = `d[0]|d[1]<<8`: **PVD = tag 1** — VolumeIdentifier dstring @24 (len 32), VolumeSetIdentifier dstring @72 (len 128), ImplementationIdentifier @388 (identifier at +1, 23 bytes, strip leading `*`); **LVD = tag 6** — LogicalVolumeIdentifier dstring @84 (len 128), DomainIdentifier UDF revision at `d[240]|d[241]<<8` (BCD → `%x.%02x`); **tag 8** = terminating descriptor (stop). UDF dstring: byte 0 = compression id (8 → 8-bit, 16 → UTF-16BE), last byte of the field = length (including the compression byte).

Fixture array names: `cdrom_getconf_wardriver[384]`, `cdrom_discinfo_wardriver[34]`, `cdrom_toc_wardriver[20]`, `iso_pvd_wardriver[2048]`, `cdrom_getconf_blank[384]`, `cdrom_discinfo_blank[34]`, `udf_nsr_lba19[2048]`, `udf_avdp_lba256[2048]`, `udf_pvd_lba32[2048]`, `udf_lvd_lba35[2048]`.

Verified formulas (hold on both discs):
- current profile = `getconf[6]<<8 | getconf[7]` (both = 0x0011 → DVD_R).
- STATE = `discinfo[2] & 3`: 0→blank, 1→appendable, 2→complete, 3→(omit).
- SESSION_COUNT = `discinfo[9]<<8 | discinfo[4]`.
- first track = `discinfo[3]`; last track = `discinfo[11]<<8 | discinfo[6]`;
  TRACK_COUNT = last − first + 1.
- TOC: 4-byte header then 8-byte descriptors; per descriptor `toc[off+1]` = ADR/control, `toc[off+2]` = track no (0xAA = lead-out, skip); `control & 0x04` → data else audio.

---

### Task 1: Media presence + type from GET CONFIGURATION current profile

**Files:**
- Modify: `cdrom_id.h` (add `cdrom_media_type`, call it from `cdrom_id_build`)
- Test: `tests/test_cdrom_media.c` (create)
- Test fixtures: `tests/fixtures/cdrom_media_wardriver.h`, `tests/fixtures/cdrom_media_blank.h` (already committed)

**Interfaces:**
- Produces: `int cdrom_media_type(const uint8_t *getconf, int len, struct uevent *out)` — reads current profile at `getconf[6..7]`; if profile ∈ {0x0000, 0xFFFF} do nothing and return 0; else `CEMIT("ID_CDROM_MEDIA")` and the matching `ID_CDROM_MEDIA_<type>` using the SAME profile→key mapping as `cdrom_id_decode`, MEDIA_ prefixed. Returns count added. Uses the existing `CEMIT` pattern (define a local `MEMIT` that appends into `out` first-writer-wins).

- [ ] **Step 1: Write the failing test** in `tests/test_cdrom_media.c`:

```c
#include "cdrom_id.h"
#include "fixtures/cdrom_media_wardriver.h"
#include "fixtures/cdrom_media_blank.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static const char *get(const struct uevent *e, const char *k) {
    const char *v = uevent_get(e, k); return v ? v : "";
}

static void test_media_type(void) {
    struct uevent e;
    cdrom_media_type(cdrom_getconf_wardriver, sizeof cdrom_getconf_wardriver, (e.n=0, &e));
    assert(strcmp(get(&e, "ID_CDROM_MEDIA"), "1") == 0);
    assert(strcmp(get(&e, "ID_CDROM_MEDIA_DVD_R"), "1") == 0);

    cdrom_media_type(cdrom_getconf_blank, sizeof cdrom_getconf_blank, (e.n=0, &e));
    assert(strcmp(get(&e, "ID_CDROM_MEDIA"), "1") == 0);
    assert(strcmp(get(&e, "ID_CDROM_MEDIA_DVD_R"), "1") == 0);
    printf("test_cdrom_media media_type: OK\n");
}

int main(void) {
    test_media_type();
    return 0;
}
```

- [ ] **Step 2: Build to verify it fails** (undefined `cdrom_media_type`):

Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm`
Expected: FAIL — implicit declaration / link error for `cdrom_media_type`.

- [ ] **Step 3: Implement `cdrom_media_type` in `cdrom_id.h`** (above `cdrom_id_build`). Reuse the profile switch from `cdrom_id_decode` but with MEDIA_ prefixed keys:

```c
static inline int cdrom_media_type(const uint8_t *buf, int len, struct uevent *out) {
    if (len < 8) return 0;
    unsigned cur = ((unsigned)buf[6] << 8) | buf[7];
    if (cur == 0x0000 || cur == 0xffff) return 0;
    #define MEMIT(k) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out, (k))) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], "1", UE_VAL_MAX); out->n++; } \
    } while (0)
    MEMIT("ID_CDROM_MEDIA");
    switch (cur) {
    case 0x08: MEMIT("ID_CDROM_MEDIA_CD"); break;
    case 0x09: MEMIT("ID_CDROM_MEDIA_CD_R"); break;
    case 0x0a: MEMIT("ID_CDROM_MEDIA_CD_RW"); break;
    case 0x10: MEMIT("ID_CDROM_MEDIA_DVD"); break;
    case 0x11: MEMIT("ID_CDROM_MEDIA_DVD_R"); break;
    case 0x12: MEMIT("ID_CDROM_MEDIA_DVD_RAM"); break;
    case 0x13: MEMIT("ID_CDROM_MEDIA_DVD_RW_RO"); break;
    case 0x14: MEMIT("ID_CDROM_MEDIA_DVD_RW_SEQ"); break;
    case 0x15: MEMIT("ID_CDROM_MEDIA_DVD_R_DL_SEQ"); break;
    case 0x16: MEMIT("ID_CDROM_MEDIA_DVD_R_DL_JR"); break;
    case 0x1a: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_RW"); break;
    case 0x1b: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_R"); break;
    case 0x2a: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_RW_DL"); break;
    case 0x2b: MEMIT("ID_CDROM_MEDIA_DVD_PLUS_R_DL"); break;
    case 0x40: MEMIT("ID_CDROM_MEDIA_BD"); break;
    case 0x41: case 0x42: MEMIT("ID_CDROM_MEDIA_BD_R"); break;
    case 0x43: MEMIT("ID_CDROM_MEDIA_BD_RE"); break;
    case 0x50: MEMIT("ID_CDROM_MEDIA_HDDVD"); break;
    case 0x51: MEMIT("ID_CDROM_MEDIA_HDDVD_R"); break;
    case 0x52: MEMIT("ID_CDROM_MEDIA_HDDVD_RW"); break;
    default: break;
    }
    #undef MEMIT
    return out->n;
}
```

Note: the real-udev DVD-R mapping for `ID_CDROM_MEDIA_<type>` uses `DVD_R` for profile 0x11 — confirmed against both fixtures. The capability decode (3d) and the media decode use independent profile fields (list vs current); do not merge them.

- [ ] **Step 4: Run test to verify it passes.** Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm` → PASS.

- [ ] **Step 5: Commit** `git add cdrom_id.h tests/test_cdrom_media.c tests/fixtures/cdrom_media_*.h && git commit -m "feat(cdrom_id): media presence + type from current profile"`

---

### Task 2: READ DISC INFO decode — state, session count, track count

**Files:** Modify `cdrom_id.h`; extend `tests/test_cdrom_media.c`.

**Interfaces:**
- Produces: `int cdrom_discinfo_decode(const uint8_t *di, int len, struct uevent *out)` — pure decode of a READ DISC INFO buffer. Emits `ID_CDROM_MEDIA` (belt-and-braces), `ID_CDROM_MEDIA_STATE`, `ID_CDROM_MEDIA_SESSION_COUNT`, `ID_CDROM_MEDIA_TRACK_COUNT`. Values are decimal strings via `snprintf`. Must NOT emit `ID_CDROM_MEDIA_SESSION_NEXT` (deferred).

- [ ] **Step 1: Write the failing test** — add to `tests/test_cdrom_media.c` and call from `main`:

```c
static void test_discinfo(void) {
    struct uevent e;
    cdrom_discinfo_decode(cdrom_discinfo_wardriver, sizeof cdrom_discinfo_wardriver, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_STATE"),"appendable")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_COUNT"),"2")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT"),"2")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_NEXT"),"")==0);   /* deferred */

    cdrom_discinfo_decode(cdrom_discinfo_blank, sizeof cdrom_discinfo_blank, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_STATE"),"blank")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_COUNT"),"1")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT"),"1")==0);
    printf("test_cdrom_media discinfo: OK\n");
}
```

- [ ] **Step 2: Build to verify it fails.** Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm` → FAIL (undefined `cdrom_discinfo_decode`).

- [ ] **Step 3: Implement in `cdrom_id.h`:**

```c
static inline int cdrom_discinfo_decode(const uint8_t *di, int len, struct uevent *out) {
    if (len < 12) return 0;
    #define DEMIT(k,v) do { \
        if (out->n < UE_MAX_KEYS && !uevent_get(out,(k))) { \
            safe_copy(out->key[out->n],(k),UE_KEY_MAX); \
            safe_copy(out->val[out->n],(v),UE_VAL_MAX); out->n++; } \
    } while (0)
    DEMIT("ID_CDROM_MEDIA", "1");
    const char *st = NULL;
    switch (di[2] & 3) { case 0: st="blank"; break; case 1: st="appendable"; break;
                         case 2: st="complete"; break; default: st=NULL; }
    if (st) DEMIT("ID_CDROM_MEDIA_STATE", st);
    char num[16];
    int sessions = (di[9] << 8) | di[4];
    snprintf(num, sizeof num, "%d", sessions); DEMIT("ID_CDROM_MEDIA_SESSION_COUNT", num);
    int first = di[3], last = (di[11] << 8) | di[6];
    int tracks = last - first + 1;
    if (tracks < 0) tracks = 0;
    snprintf(num, sizeof num, "%d", tracks); DEMIT("ID_CDROM_MEDIA_TRACK_COUNT", num);
    #undef DEMIT
    return out->n;
}
```

- [ ] **Step 4: Run test to verify it passes.** Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm` → PASS.

- [ ] **Step 5: Commit** `git add cdrom_id.h tests/test_cdrom_media.c && git commit -m "feat(cdrom_id): READ DISC INFO state/session/track decode"`

---

### Task 3: READ TOC decode — data/audio track split

**Files:** Modify `cdrom_id.h`; extend `tests/test_cdrom_media.c`.

**Interfaces:**
- Produces: `int cdrom_toc_decode(const uint8_t *toc, int len, struct uevent *out)` — walks 8-byte descriptors from offset 4; counts data (`control & 0x04`) vs audio, skipping track 0xAA. Emits `ID_CDROM_MEDIA_TRACK_COUNT_DATA` / `_AUDIO` **only when the respective count > 0**.

- [ ] **Step 1: Write the failing test** — add and call from `main`:

```c
static void test_toc(void) {
    struct uevent e;
    cdrom_toc_decode(cdrom_toc_wardriver, sizeof cdrom_toc_wardriver, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT_DATA"),"1")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT_AUDIO"),"")==0);  /* 0 -> omitted */
    printf("test_cdrom_media toc: OK\n");
}
```

- [ ] **Step 2: Build to verify it fails.** Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm` → FAIL.

- [ ] **Step 3: Implement in `cdrom_id.h`:**

```c
static inline int cdrom_toc_decode(const uint8_t *toc, int len, struct uevent *out) {
    if (len < 4) return 0;
    int ndata = 0, naudio = 0;
    for (int off = 4; off + 8 <= len; off += 8) {
        if (toc[off + 2] == 0xaa) continue;           /* lead-out */
        if (toc[off + 1] & 0x04) ndata++; else naudio++;
    }
    char num[16];
    if (ndata > 0)  { snprintf(num,sizeof num,"%d",ndata);
        if (!uevent_get(out,"ID_CDROM_MEDIA_TRACK_COUNT_DATA") && out->n < UE_MAX_KEYS) {
            safe_copy(out->key[out->n],"ID_CDROM_MEDIA_TRACK_COUNT_DATA",UE_KEY_MAX);
            safe_copy(out->val[out->n],num,UE_VAL_MAX); out->n++; } }
    if (naudio > 0) { snprintf(num,sizeof num,"%d",naudio);
        if (!uevent_get(out,"ID_CDROM_MEDIA_TRACK_COUNT_AUDIO") && out->n < UE_MAX_KEYS) {
            safe_copy(out->key[out->n],"ID_CDROM_MEDIA_TRACK_COUNT_AUDIO",UE_KEY_MAX);
            safe_copy(out->val[out->n],num,UE_VAL_MAX); out->n++; } }
    return out->n;
}
```

- [ ] **Step 4: Run test to verify it passes.** → PASS.

- [ ] **Step 5: Commit** `git add cdrom_id.h tests/test_cdrom_media.c && git commit -m "feat(cdrom_id): READ TOC data/audio track decode"`

---

### Task 4: Media-ready gate + SG_IO wiring + orchestration

**Files:** Modify `cdrom_id.h`. (No unit test — SG_IO needs hardware; exercised by the live gate in Task 7.)

**Interfaces:**
- Produces: `int cdrom_test_unit_ready(int fd)` → returns 1 ready, 0 not-ready/no-media.
  `int cdrom_read_disc_info(int fd, uint8_t *buf, size_t sz, int *len)` (cdb 0x51).
  `int cdrom_read_toc(int fd, uint8_t *buf, size_t sz, int *len)` (cdb 0x43 fmt 0).
- Consumes: `cdrom_media_type`, `cdrom_discinfo_decode`, `cdrom_toc_decode` (Tasks 1–3), `optical_fs_probe` (Task 5, add its call in Task 5).

- [ ] **Step 1: Add a shared SG_IO helper** in `cdrom_id.h` (factor the existing pattern in `cdrom_get_config` — refactor `cdrom_get_config` to use it too, DRY):

```c
static inline int cdrom_sg(int fd, const uint8_t *cdb, int cdblen,
                           uint8_t *buf, int buflen, int dir) {
    uint8_t sense[32] = {0};
    struct sg_io_hdr io = {0};
    io.interface_id='S'; io.dxfer_direction=dir; io.cmd_len=cdblen;
    io.cmdp=(uint8_t*)cdb; io.dxfer_len=buflen; io.dxferp=buf;
    io.sbp=sense; io.mx_sb_len=sizeof sense; io.timeout=8000;
    if (ioctl(fd, SG_IO, &io) < 0) return -2;
    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
    return buflen - io.resid;
}
```

- [ ] **Step 2: Add TUR + reads:**

```c
#include <time.h>
static inline int cdrom_test_unit_ready(int fd) {
    uint8_t cdb[6] = {0,0,0,0,0,0};
    for (int i = 0; i < 5; i++) {
        if (cdrom_sg(fd, cdb, 6, NULL, 0, SG_DXFER_NONE) == 0) return 1;
        struct timespec ts = {0, 200*1000*1000L};   /* 200 ms */
        nanosleep(&ts, NULL);
    }
    return 0;
}
static inline int cdrom_read_disc_info(int fd, uint8_t *buf, size_t sz, int *len) {
    uint8_t cdb[10] = {0x51,0,0,0,0,0,0,(uint8_t)(sz>>8),(uint8_t)(sz&0xff),0};
    int r = cdrom_sg(fd, cdb, 10, buf, (int)sz, SG_DXFER_FROM_DEV);
    if (r < 4) return -1; *len = r; return 0;
}
static inline int cdrom_read_toc(int fd, uint8_t *buf, size_t sz, int *len) {
    uint8_t cdb[10] = {0x43,0,0,0,0,0,1,(uint8_t)(sz>>8),(uint8_t)(sz&0xff),0};
    int r = cdrom_sg(fd, cdb, 10, buf, (int)sz, SG_DXFER_FROM_DEV);
    if (r < 4) return -1; *len = r; return 0;
}
```

- [ ] **Step 3: Rewrite `cdrom_id_build` to orchestrate.** Keep the existing GET CONFIG capability decode; then open the device once for TUR + media reads. If TUR is not ready, emit capabilities + media-type only and stop (no disc info / toc / fs):

```c
static inline int cdrom_id_build(const char *sysroot, const char *devpath,
                                 const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    uint8_t buf[2048]; int len = 0;
    if (cdrom_get_config(devnode, buf, sizeof buf, &len) == 0)
        cdrom_id_decode(buf, len, out);            /* 3d capabilities (resets out->n) */
    /* NOTE: cdrom_id_decode sets out->n=0 at entry; keep it FIRST. */
    cdrom_media_type(buf, len, out);               /* 3e media presence + type */

    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return out->n;
    if (!cdrom_test_unit_ready(fd)) { close(fd); return out->n; }

    uint8_t di[64]; int dlen = 0;
    if (cdrom_read_disc_info(fd, di, sizeof di, &dlen) == 0)
        cdrom_discinfo_decode(di, dlen, out);
    uint8_t tc[64]; int tlen = 0;
    if (cdrom_read_toc(fd, tc, sizeof tc, &tlen) == 0)
        cdrom_toc_decode(tc, tlen, out);
    close(fd);

    optical_fs_probe(devnode, out);                /* 3e optical filesystem (Task 5) */
    return out->n;
}
```

**IMPORTANT:** `cdrom_id_decode` resets `out->n = 0` at entry — it must be the first thing called, before `cdrom_media_type`. `optical_fs_probe` must append (first-writer-wins), never reset. `#include "optical_fs.h"` at the top of `cdrom_id.h` (add in Task 5). Until Task 5 lands, stub `optical_fs_probe` is not present — add the call line in Task 5, not here. For THIS task, omit the `optical_fs_probe(...)` line and its include; add them in Task 5.

- [ ] **Step 4: Build the whole tree to verify it compiles.** Run: `make` (or `make schema-udev`). Expected: clean build. Run `make test` → existing tests still green.

- [ ] **Step 5: Commit** `git add cdrom_id.h && git commit -m "feat(cdrom_id): TEST UNIT READY gate + disc-info/toc SG_IO orchestration"`

---

### Task 5: optical_fs.h — ISO9660 (salvage) + UDF prober

**Files:** Create `optical_fs.h`; modify `cdrom_id.h` (include + call); extend `tests/test_cdrom_media.c`.

**Interfaces:**
- Produces: `int optical_fs_probe(const char *devnode, struct uevent *out)` — tries ISO9660 then UDF; first hit appends `ID_FS_*` and returns 0; none → returns -1, appends nothing.
- Consumes: `bpt_read_at`, `bpt_emit`, `fs_emit_label`, `fs_emit_uuid`, `fs_safe_bytes`, `fs_encode_bytes` from `blkid_fs.h` (on master).

- [ ] **Step 1: Salvage the ISO9660 prober.** Copy VERBATIM from the pre-pare-back commit into `optical_fs.h`:

```
git show 0652c65:blkid_fs.h | sed -n '/static inline void fs_trim_bytes/,/^}$/p'      # fs_trim_bytes
git show 0652c65:blkid_fs.h | sed -n '/static inline void fs_trim_encode_bytes/,/^}$/p'
git show 0652c65:blkid_fs.h | sed -n '/static inline int fs_probe_iso9660/,/^}$/p'
```
Put all three (`fs_trim_bytes`, `fs_trim_encode_bytes`, `fs_probe_iso9660`) into `optical_fs.h`. Header skeleton:

```c
#ifndef OPTICAL_FS_H
#define OPTICAL_FS_H
#include "schema-udev.h"
#include "blkid_fs.h"     /* bpt_read_at, bpt_emit, fs_emit_label/uuid, fs_safe/encode_bytes */
#include <string.h>
#include <stdint.h>
/* --- salvaged verbatim from 0652c65:blkid_fs.h --- */
/* fs_trim_bytes, fs_trim_encode_bytes, fs_probe_iso9660 here */
/* --- UDF (Step 3) --- */
/* --- optical_fs_probe (Step 4) --- */
#endif
```

- [ ] **Step 2: Write the failing ISO9660 test** — add to `tests/test_cdrom_media.c`. It writes the PVD fixture into a temp file at offset 32768 (the real prober reads via `bpt_read_at`, which works on a regular file), then calls the production prober:

```c
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
static void test_iso9660(void) {
    char path[] = "/tmp/optfsXXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(ftruncate(fd, 32768 + 2048) == 0);
    assert(pwrite(fd, iso_pvd_wardriver, sizeof iso_pvd_wardriver, 32768)
           == (ssize_t)sizeof iso_pvd_wardriver);
    close(fd);
    struct uevent e; e.n = 0;
    assert(optical_fs_probe(path, &e) == 0);
    assert(strcmp(get(&e,"ID_FS_TYPE"),"iso9660")==0);
    assert(strcmp(get(&e,"ID_FS_LABEL"),"Wardriver.2026.1080p.WEBRip.x264")==0);
    assert(strcmp(get(&e,"ID_FS_SYSTEM_ID"),"LINUX")==0);
    assert(strcmp(get(&e,"ID_FS_UUID"),"2026-05-09-01-34-23-00")==0);
    assert(strstr(get(&e,"ID_FS_APPLICATION_ID"),"K3B") != NULL);
    unlink(path);
    printf("test_cdrom_media iso9660: OK\n");
}
```
(The fixture is PVD-only; `ID_FS_VERSION`=Joliet needs the SVD and is covered by the live gate, not asserted here.)
Add `#include "optical_fs.h"` to the test if not already pulled via `cdrom_id.h`, and call `test_iso9660()` from `main`.

- [ ] **Step 3: Add the full UDF prober** `fs_probe_udf` (+ `udf_dstring` helper) to `optical_fs.h`. Detect via the VRS, then parse AVDP@256 → Main VDS → PVD/LVD. All offsets verified against `POWERT_TOUR_DVD` (see ground-truth section). Never fabricate — emit each field only when present:

```c
/* UDF dstring: field[0]=compression id, field[fieldlen-1]=length (incl comp byte);
   comp 8 = 8-bit chars, comp 16 = UTF-16BE, both starting at field[1]. */
static inline void udf_dstring(const unsigned char *f, int fieldlen, char *out, int outsz) {
    int len = f[fieldlen - 1], o = 0;
    if (len <= 1) { out[0] = 0; return; }
    if (f[0] == 16) { for (int i = 1; i + 1 < len && o < outsz - 1; i += 2) out[o++] = f[i + 1]; }
    else            { for (int i = 1; i < len && o < outsz - 1; i++)     out[o++] = f[i]; }
    out[o] = 0;
}

static inline int fs_probe_udf(const char *dev, struct uevent *out) {
    unsigned char d[2048];
    int found = 0;
    for (int sec = 16; sec <= 20; sec++) {
        if (bpt_read_at(dev, (uint64_t)sec * 2048, d, sizeof d) != 0) break;
        if (memcmp(d + 1, "NSR02", 5) == 0 || memcmp(d + 1, "NSR03", 5) == 0) { found = 1; break; }
        if (memcmp(d + 1, "TEA01", 5) == 0) break;      /* end of VRS */
    }
    if (!found) return -1;
    bpt_emit(out, "ID_FS_TYPE", "udf");
    bpt_emit(out, "ID_FS_USAGE", "filesystem");

    if (bpt_read_at(dev, 256ULL * 2048, d, sizeof d) != 0) return 0;   /* AVDP */
    if ((d[0] | (d[1] << 8)) != 2) return 0;
    uint32_t loc = d[20] | (d[21]<<8) | (d[22]<<16) | ((uint32_t)d[23]<<24);
    uint32_t mlen = d[16] | (d[17]<<8) | (d[18]<<16) | ((uint32_t)d[19]<<24);
    uint32_t nsec = mlen / 2048; if (nsec > 64) nsec = 64;

    char label[128]="", volid[64]="", volset[128]="", appid[128]="", version[8]="";
    for (uint32_t i = 0; i <= nsec; i++) {
        if (bpt_read_at(dev, (uint64_t)(loc + i) * 2048, d, sizeof d) != 0) break;
        unsigned t = d[0] | (d[1] << 8);
        if (t == 1) {                                    /* PVD */
            udf_dstring(d + 24, 32, volid, sizeof volid);
            udf_dstring(d + 72, 128, volset, sizeof volset);
            const unsigned char *impl = d + 388 + 1;     /* ImplId identifier (skip flags) */
            size_t ilen = strnlen((const char *)impl, 23);
            if (ilen && impl[0] == '*') { impl++; ilen--; }
            fs_encode_bytes(impl, ilen, appid, sizeof appid);
        } else if (t == 6) {                             /* LVD */
            udf_dstring(d + 84, 128, label, sizeof label);
            unsigned rev = d[240] | (d[241] << 8);
            snprintf(version, sizeof version, "%x.%02x", rev >> 8, rev & 0xff);
        } else if (t == 8) break;                        /* terminating descriptor */
    }
    if (label[0])  { fs_emit_label(out, (const unsigned char *)label, strlen(label));
                     bpt_emit(out, "ID_FS_LOGICAL_VOLUME_ID", label); }
    if (volid[0])  bpt_emit(out, "ID_FS_VOLUME_ID", volid);
    if (volset[0]) bpt_emit(out, "ID_FS_VOLUME_SET_ID", volset);
    if (version[0] && strcmp(version, "0.00") != 0) bpt_emit(out, "ID_FS_VERSION", version);
    if (appid[0])  bpt_emit(out, "ID_FS_APPLICATION_ID", appid);
    if (volset[0]) {                                      /* UUID: lowercase volset, pad to 16 */
        char uuid[17]; int j;
        for (j = 0; j < 16 && volset[j]; j++) {
            char c = volset[j];
            uuid[j] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        for (; j < 16; j++) uuid[j] = '0';
        uuid[16] = 0;
        fs_emit_uuid(out, uuid);
    }
    return 0;
}
```

Expected against `POWERT_TOUR_DVD`: `ID_FS_TYPE=udf`, `ID_FS_LABEL`/`_ENC`/`ID_FS_LOGICAL_VOLUME_ID`/`ID_FS_VOLUME_ID`=`POWERT_TOUR_DVD`, `ID_FS_VOLUME_SET_ID=3655822E`, `ID_FS_UUID`/`_ENC`=`3655822e00000000`, `ID_FS_VERSION=1.02`, `ID_FS_APPLICATION_ID=Apple\x20Computer\x2c\x20Inc.`.

- [ ] **Step 4: Add `optical_fs_probe`** and wire it into `cdrom_id.h`:

```c
static inline int optical_fs_probe(const char *devnode, struct uevent *out) {
    if (fs_probe_udf(devnode, out) == 0) return 0;       /* UDF wins on bridge discs */
    if (fs_probe_iso9660(devnode, out) == 0) return 0;
    return -1;
}
```
**Ordering matters:** UDF is tried FIRST. Real udev reports `ID_FS_TYPE=udf` for a UDF+ISO9660 bridge disc even though the ISO9660 PVD is readable; ISO9660-first would mislabel it and break parity. A pure-ISO9660 disc has no UDF VRS, so `fs_probe_udf` returns -1 and it falls through correctly.

In `cdrom_id.h`: add `#include "optical_fs.h"` near the top, and add the `optical_fs_probe(devnode, out);` line at the end of `cdrom_id_build` (the spot noted in Task 4).

- [ ] **Step 5: Add the real-fixture UDF test** to `tests/test_cdrom_media.c`. Write the 4 captured sectors to their real LBA offsets in a sparse temp file (`ftruncate` makes the gaps sparse), then run the production `optical_fs_probe` and assert the full key set including that UDF wins over the bridge disc's ISO9660:

```c
#include "fixtures/cdrom_media_udf.h"
static void test_udf(void) {
    char path[] = "/tmp/optudfXXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(ftruncate(fd, 257ULL*2048) == 0);
    assert(pwrite(fd, udf_nsr_lba19,   2048, 19ULL*2048)  == 2048);
    assert(pwrite(fd, udf_avdp_lba256, 2048, 256ULL*2048) == 2048);
    assert(pwrite(fd, udf_pvd_lba32,   2048, 32ULL*2048)  == 2048);
    assert(pwrite(fd, udf_lvd_lba35,   2048, 35ULL*2048)  == 2048);
    close(fd);
    struct uevent e; e.n = 0;
    assert(optical_fs_probe(path, &e) == 0);
    assert(strcmp(get(&e,"ID_FS_TYPE"),"udf")==0);
    assert(strcmp(get(&e,"ID_FS_LABEL"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_LOGICAL_VOLUME_ID"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_VOLUME_ID"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_VOLUME_SET_ID"),"3655822E")==0);
    assert(strcmp(get(&e,"ID_FS_UUID"),"3655822e00000000")==0);
    assert(strcmp(get(&e,"ID_FS_VERSION"),"1.02")==0);
    assert(strstr(get(&e,"ID_FS_APPLICATION_ID"),"Apple")!=NULL);
    unlink(path);
    printf("test_cdrom_media udf: OK\n");
}
```
Call `test_udf()` from `main`. (The AVDP's Main-VDS location is the absolute LBA 32, which is exactly where `udf_pvd_lba32` is written, so the sparse file resolves correctly.)

- [ ] **Step 6: Build + run.** Run: `cc -I. -o /tmp/tcm tests/test_cdrom_media.c && /tmp/tcm` → all subtests PASS. Then `make` → clean.

- [ ] **Step 7: Commit** `git add optical_fs.h cdrom_id.h tests/test_cdrom_media.c && git commit -m "feat(optical_fs): salvage ISO9660 + add UDF prober; wire into cdrom_id"`

---

### Task 6: Parity classifier — bring media + optical FS in scope

**Files:** Modify `udev-parity.h`.

**Interfaces:** none new — edits existing predicates.

- [ ] **Step 1: Remove the two 3e deferrals.** In `udev-parity.h`:
  - Delete `parity_cdrom_media` (function + its call in the block branch of `parity_in_scope_missing`).
  - Delete `parity_is_optical` (function + the `if (parity_is_optical(devpath) && strncmp(key,"ID_FS_",6)==0) return 0;` line).

- [ ] **Step 2: Add the SESSION_NEXT deferral.** In `parity_deferred`, add:
```c
           strcmp(key, "ID_CDROM_MEDIA_SESSION_NEXT") == 0 ||
```
(so real udev's SESSION_NEXT does not count as an in-scope gap — documented deferral: needs READ TRACK INFORMATION for correct cross-state semantics).

- [ ] **Step 3: Build.** Run: `make parity` → compiles clean.

- [ ] **Step 4: Verify parity is 0/0** with a disc inserted (requires hardware — the reviewer/Claire runs this under sudo; Greg notes it as a hardware gate):
Run: `sudo ./udev-parity` → 0 missing / 0 extra, both directions.

- [ ] **Step 5: Commit** `git add udev-parity.h && git commit -m "feat(cdrom_id): bring ID_CDROM_MEDIA* + optical ID_FS_ in-scope; defer SESSION_NEXT"`

---

### Task 7: Live gate + Makefile wiring

**Files:** Create `tests/verify_cdrom_media_live.sh`; modify `Makefile`.

**Interfaces:** none.

- [ ] **Step 1: Makefile.** Add `tests/test_cdrom_media` to the unit-test target list (mirror how `tests/test_udev_builtins` is built and run). It compiles with `cc -I. -o tests/test_cdrom_media tests/test_cdrom_media.c` and runs in the `test` target.

- [ ] **Step 2: Write `tests/verify_cdrom_media_live.sh`** — asserts the DAEMON's shadow db (`/run/schema-udev/data/b11:0`), NOT the warm parity tool. It must:
  - resolve `MM=$(cat /sys/block/sr0/dev)`;
  - ensure the daemon has processed sr0 (trigger its coldplug/uevent path for sr0), then read `/run/schema-udev/data/b$MM`;
  - with the **burned** disc inserted, assert the record CONTAINS (positive grep, anti-hollow): `ID_CDROM_MEDIA=1`, `ID_CDROM_MEDIA_STATE=appendable`, `ID_CDROM_MEDIA_TRACK_COUNT=2`, `ID_CDROM_MEDIA_TRACK_COUNT_DATA=1`, `ID_FS_TYPE=iso9660`, `ID_FS_LABEL=Wardriver.2026.1080p.WEBRip.x264`, `ID_FS_VERSION=Joliet Extension`;
  - with the **UDF bridge** disc (`POWERT_TOUR_DVD`) inserted, assert the daemon record has `ID_FS_TYPE=udf` (NOT iso9660), `ID_FS_LABEL=POWERT_TOUR_DVD`, `ID_FS_VERSION=1.02`, and `ID_CDROM_MEDIA_STATE=complete`;
  - **inverted-#94 regression:** with NO media (empty drive), assert the daemon record has ZERO `ID_FS_` lines (`! grep -q '^E:ID_FS_'`);
  - the burned/UDF/empty passes are each prompted (disc swap) or guarded by a detected current-profile/media-state so the script asserts against the disc actually present;
  - print explicit PASS/FAIL per assertion and exit non-zero on any miss.
  Use `printf`, `grep -q`, and assert the tool's own found/absent state — never `grep -v ... | wc -l == 0`.

```sh
#!/bin/sh
set -eu
DEV=/dev/sr0
MM=$(cat /sys/block/sr0/dev)
REC=/run/schema-udev/data/b$MM
fail() { echo "FAIL: $1"; exit 1; }
have() { grep -q "^E:$1" "$REC" || fail "missing $1 in $REC"; echo "ok: $1"; }
# (daemon coldplug trigger for sr0 goes here — match the project's live-gate convention)
echo "== burned disc =="
have "ID_CDROM_MEDIA=1"
have "ID_CDROM_MEDIA_STATE=appendable"
have "ID_CDROM_MEDIA_TRACK_COUNT=2"
have "ID_CDROM_MEDIA_TRACK_COUNT_DATA=1"
have "ID_FS_TYPE=iso9660"
have "ID_FS_LABEL=Wardriver.2026.1080p.WEBRip.x264"
have "ID_FS_VERSION=Joliet Extension"
echo "PASS burned"
```
(The empty-drive regression block and the daemon-trigger mechanism follow the same convention as `tests/verify_cdrom_id_live.sh` from #94 — model them on it.)

- [ ] **Step 3: Build + run unit tests.** Run: `make test` → all green including `test_cdrom_media`.

- [ ] **Step 4: Commit** `git add tests/verify_cdrom_media_live.sh Makefile && git commit -m "test(cdrom_id): media live gate (daemon shadow db) + Makefile wiring"`

---

## Self-review notes

- **Spec coverage:** media-ready (T4), media presence/type (T1), state+counts (T2/T3), optical FS ISO9660+UDF (T5), parity in-scope + SESSION_NEXT deferral (T6), daemon-shadow-db live gate + inverted-#94 regression (T7). All spec sections mapped.
- **SESSION_NEXT:** deferred by decision — real-udev emits it but the formula is under-determined by the two available discs; deferral keeps parity honest at 0/0.
- **No `schema-udev.c` / `ub_select` change** — cdrom_id already fires on sr*/scd*; the diff must show `schema-udev.c` and `udev_builtins.h` untouched.
- **UDF** is a full parse (AVDP→PVD/LVD), verified live against the `POWERT_TOUR_DVD` bridge disc; fixture `cdrom_media_udf.h` holds the 4 real sectors. Prober order is UDF-first (bridge discs report udf). All offsets confirmed against the real udev oracle.
- **Order dependency:** `cdrom_id_decode` (resets out->n) must run before `cdrom_media_type`; `optical_fs_probe` appends only. Flagged in T4/T5.
