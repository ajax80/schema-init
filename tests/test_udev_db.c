#include "../schema-udev.h"
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

    /* char device: mem/null -> c1:3 */
    struct uevent c; memset(&c, 0, sizeof c);
    put(&c, "SUBSYSTEM", "mem"); put(&c, "MAJOR", "1"); put(&c, "MINOR", "3");
    put(&c, "DEVPATH", "/devices/virtual/mem/null");
    assert(udev_db_filename(&c, name, sizeof name) == 0);
    assert(strcmp(name, "c1:3") == 0);

    /* block device -> b8:0 */
    struct uevent b; memset(&b, 0, sizeof b);
    put(&b, "SUBSYSTEM", "block"); put(&b, "MAJOR", "8"); put(&b, "MINOR", "0");
    assert(udev_db_filename(&b, name, sizeof name) == 0);
    assert(strcmp(name, "b8:0") == 0);

    /* net device -> n2 */
    struct uevent ndev; memset(&ndev, 0, sizeof ndev);
    put(&ndev, "SUBSYSTEM", "net"); put(&ndev, "IFINDEX", "2");
    assert(udev_db_filename(&ndev, name, sizeof name) == 0);
    assert(strcmp(name, "n2") == 0);

    /* no devnum -> +subsys:sysname (basename of DEVPATH) */
    struct uevent o; memset(&o, 0, sizeof o);
    put(&o, "SUBSYSTEM", "acpi"); put(&o, "DEVPATH", "/devices/LNXSYSTM:00/AMDI0030:00");
    assert(udev_db_filename(&o, name, sizeof name) == 0);
    assert(strcmp(name, "+acpi:AMDI0030:00") == 0);

    /* record contents */
    char rec[4096];
    ssize_t rn = udev_db_record_build(&c, rec, sizeof rec);
    assert(rn > 0);
    assert(strcmp(rec,
        "V:1\n"
        "E:SUBSYSTEM=mem\n"
        "E:MAJOR=1\n"
        "E:MINOR=3\n"
        "E:DEVPATH=/devices/virtual/mem/null\n") == 0);

    /* write to a /tmp base -> file named c1:3 with the record */
    char tmpl[] = "/tmp/schema-udev-db-XXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base);
    assert(udev_db_write(base, &c) == 0);
    char path[256]; snprintf(path, sizeof path, "%s/c1:3", base);
    FILE *f = fopen(path, "r"); assert(f);
    char got[4096]; size_t gl = fread(got, 1, sizeof got - 1, f); got[gl] = '\0'; fclose(f);
    assert(strncmp(got, "V:1\n", 4) == 0);
    unlink(path); rmdir(base);

    /* overflow -> -1 */
    assert(udev_db_record_build(&c, rec, 3) == -1);

    printf("test_udev_db: OK\n");
    return 0;
}
