#include "../udev_db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    char name[128];

    struct uevent c; memset(&c, 0, sizeof c);
    put(&c, "SUBSYSTEM", "mem"); put(&c, "MAJOR", "1"); put(&c, "MINOR", "3");
    put(&c, "DEVPATH", "/devices/virtual/mem/null");
    assert(udev_db_filename(&c, name, sizeof name) == 0 && strcmp(name, "c1:3") == 0);

    struct uevent b; memset(&b, 0, sizeof b);
    put(&b, "SUBSYSTEM", "block"); put(&b, "MAJOR", "8"); put(&b, "MINOR", "0");
    assert(udev_db_filename(&b, name, sizeof name) == 0 && strcmp(name, "b8:0") == 0);

    struct uevent ndev; memset(&ndev, 0, sizeof ndev);
    put(&ndev, "SUBSYSTEM", "net"); put(&ndev, "IFINDEX", "2");
    assert(udev_db_filename(&ndev, name, sizeof name) == 0 && strcmp(name, "n2") == 0);

    struct uevent o; memset(&o, 0, sizeof o);
    put(&o, "SUBSYSTEM", "acpi"); put(&o, "DEVPATH", "/devices/LNXSYSTM:00/AMDI0030:00");
    assert(udev_db_filename(&o, name, sizeof name) == 0 && strcmp(name, "+acpi:AMDI0030:00") == 0);

    /* delta serialization: kernel props [0,kernel_n), derived props after */
    struct uevent d; memset(&d, 0, sizeof d);
    put(&d, "SUBSYSTEM", "block"); put(&d, "MAJOR", "8"); put(&d, "MINOR", "0");
    put(&d, "DEVPATH", "/devices/x");
    int kernel_n = d.n;
    put(&d, "ID_FS_TYPE", "ext4"); put(&d, "ID_FS_UUID", "abc");
    char rec[4096];
    ssize_t rn = udev_db_record_build(&d, kernel_n, rec, sizeof rec);
    assert(rn > 0);
    assert(strcmp(rec, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=abc\nV:1\n") == 0);
    assert(strstr(rec, "E:SUBSYSTEM=") == NULL);
    assert(strstr(rec, "E:MAJOR=") == NULL);
    assert(strstr(rec, "E:DEVPATH=") == NULL);
    /* V:1 is the trailing line */
    assert(strcmp(rec + rn - 4, "V:1\n") == 0);

    /* empty value skipped */
    struct uevent e; memset(&e, 0, sizeof e);
    int en = e.n;
    put(&e, "ID_A", "x"); put(&e, "ID_EMPTY", ""); put(&e, "ID_B", "y");
    ssize_t ern = udev_db_record_build(&e, en, rec, sizeof rec);
    assert(ern > 0 && strstr(rec, "ID_EMPTY") == NULL);
    assert(strstr(rec, "E:ID_A=x\n") && strstr(rec, "E:ID_B=y\n"));

    /* dotted private props are never persisted (matches udev: 0 dotted E: in /run/udev/data) */
    struct uevent dp; memset(&dp, 0, sizeof dp);
    int dpn = dp.n;
    put(&dp, "ID_REAL", "1"); put(&dp, ".PART_SUFFIX", "-part1"); put(&dp, ".HAVE_HWDB_PROPERTIES", "1");
    ssize_t dprn = udev_db_record_build(&dp, dpn, rec, sizeof rec);
    assert(dprn > 0);
    assert(strstr(rec, "E:ID_REAL=1\n") != NULL);
    assert(strstr(rec, ".PART_SUFFIX") == NULL);
    assert(strstr(rec, ".HAVE_HWDB_PROPERTIES") == NULL);

    /* write -> read-back round-trips the derived E: set */
    char tmpl[] = "/tmp/schema-udev-db-XXXXXX";
    char *base = mkdtemp(tmpl); assert(base);
    assert(udev_db_write(base, &d, kernel_n) == 0);
    char path[256]; snprintf(path, sizeof path, "%s/b8:0", base);
    struct uevent got; assert(udev_db_read_eprops(path, &got) == 0);
    assert(got.n == 2);
    assert(strcmp(uevent_get(&got, "ID_FS_TYPE"), "ext4") == 0);
    assert(strcmp(uevent_get(&got, "ID_FS_UUID"), "abc") == 0);

    /* overflow -> -1, nothing usable */
    assert(udev_db_record_build(&d, kernel_n, rec, 3) == -1);

    /* full record: S: symlinks, I: init usec, E: props, G:/Q: tags, V: last */
    const char *syms[] = { "disk/by-id/foo", "disk/by-diskseq/24" };
    const char *tags[] = { "uaccess", "systemd" };
    ssize_t fn = udev_db_record_build_full(&d, kernel_n, syms, 2, 3205703LL,
                                           tags, 2, rec, sizeof rec);
    assert(fn > 0);
    assert(strcmp(rec,
        "S:disk/by-id/foo\n"
        "S:disk/by-diskseq/24\n"
        "I:3205703\n"
        "E:ID_FS_TYPE=ext4\n"
        "E:ID_FS_UUID=abc\n"
        "G:uaccess\n"
        "G:systemd\n"
        "Q:uaccess\n"
        "Q:systemd\n"
        "V:1\n") == 0);
    /* no symlinks -> no S: lines; usec 0 -> no I: line; no tags -> no G:/Q: */
    ssize_t fn2 = udev_db_record_build_full(&d, kernel_n, NULL, 0, 0,
                                            NULL, 0, rec, sizeof rec);
    assert(fn2 > 0);
    assert(strcmp(rec, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=abc\nV:1\n") == 0);
    assert(strstr(rec, "S:") == NULL && strstr(rec, "I:") == NULL &&
           strstr(rec, "G:") == NULL && strstr(rec, "Q:") == NULL);

    /* remove unlinks the record; a second remove is still success (ENOENT) */
    assert(udev_db_remove(base, &d) == 0);
    struct uevent gone;
    assert(udev_db_read_eprops(path, &gone) != 0);   /* file is gone */
    assert(udev_db_remove(base, &d) == 0);            /* idempotent */

    unlink(path); rmdir(base);

    /* --- R5: udev_db_write_full round-trip (S:/G:/Q:/E:) --- */
    char tmpl2[] = "/tmp/schema-r5-XXXXXX";
    char *rbase = mkdtemp(tmpl2);
    assert(rbase);

    struct uevent fe; memset(&fe, 0, sizeof fe);
    put(&fe, "SUBSYSTEM", "block"); put(&fe, "MAJOR", "8"); put(&fe, "MINOR", "0");
    put(&fe, "DEVPATH", "/devices/x/block/sda");
    int fkn = fe.n;                       /* kernel boundary */
    put(&fe, "ID_FS_TYPE", "ext4");       /* a derived E: prop */
    const char *fsyms[] = { "disk/by-id/wwn-0xabc", "disk/by-path/pci-0000" };
    const char *ftags[] = { "systemd", "seat" };
    assert(udev_db_write_full(rbase, &fe, fkn, fsyms, 2, ftags, 2) == 0);

    char fpath[512];
    snprintf(fpath, sizeof fpath, "%s/b8:0", rbase);
    char links[8][UE_VAL_MAX]; int nl = 0;
    char rtags[8][UE_KEY_MAX];  int nt = 0;
    assert(udev_db_read_links_tags(fpath, links, &nl, 8, rtags, &nt, 8) == 0);
    assert(nl == 2 && !strcmp(links[0], "disk/by-id/wwn-0xabc") && !strcmp(links[1], "disk/by-path/pci-0000"));
    assert(nt == 2 && !strcmp(rtags[0], "systemd") && !strcmp(rtags[1], "seat"));

    /* E: delta present, G: and Q: both emitted, V: last */
    struct uevent fback; assert(udev_db_read_eprops(fpath, &fback) == 0);
    assert(!strcmp(uevent_get(&fback, "ID_FS_TYPE"), "ext4"));

    /* Part A: every written record carries an I: init-usec line (is_initialized) */
    FILE *rf = fopen(fpath, "r"); assert(rf);
    char rline[1024]; long long iusec = -1;
    while (fgets(rline, sizeof rline, rf))
        if (rline[0] == 'I' && rline[1] == ':') { iusec = atoll(rline + 2); break; }
    fclose(rf);
    assert(iusec > 0);   /* real CLOCK_MONOTONIC usec, not omitted */
    unlink(fpath); rmdir(rbase);

    printf("test_udev_db: OK\n");
    return 0;
}
