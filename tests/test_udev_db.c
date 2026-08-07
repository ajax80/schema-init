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

    /* remove unlinks the record; a second remove is still success (ENOENT) */
    assert(udev_db_remove(base, &d) == 0);
    struct uevent gone;
    assert(udev_db_read_eprops(path, &gone) != 0);   /* file is gone */
    assert(udev_db_remove(base, &d) == 0);            /* idempotent */

    unlink(path); rmdir(base);
    printf("test_udev_db: OK\n");
    return 0;
}
