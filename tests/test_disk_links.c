#include "../disk_links.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void put(struct uevent *e, const char *k, const char *v) {
    safe_copy(e->key[e->n], k, UE_KEY_MAX);
    safe_copy(e->val[e->n], v, UE_VAL_MAX);
    e->n++;
}

static void assert_link(const char *base, const char *tree, const char *name,
                        const char *want_target) {
    char lp[1024];
    snprintf(lp, sizeof lp, "%s/%s/%s", base, tree, name);
    char tgt[512];
    ssize_t l = readlink(lp, tgt, sizeof tgt - 1);
    assert(l > 0);
    tgt[l] = '\0';
    assert(strcmp(tgt, want_target) == 0);
}

int main(void) {
    /* ---- apply: whole disk ---- */
    char t1[] = "/tmp/schema-dl-XXXXXX";
    char *root = mkdtemp(t1); assert(root);
    char base[512]; snprintf(base, sizeof base, "%s/disk", root);

    struct uevent d; d.n = 0;
    put(&d, "SUBSYSTEM", "block");
    put(&d, "DEVNAME", "sda");
    put(&d, "DEVTYPE", "disk");
    put(&d, "MAJOR", "8"); put(&d, "MINOR", "0");
    put(&d, "DISKSEQ", "2");
    put(&d, "ID_FS_UUID_ENC", "e841ba0a-d7b9-42b6-b627-8ea27df85a54");
    put(&d, "ID_PATH", "pci-0000:02:00.1-ata-1.0");

    assert(disk_links_apply(base, &d) == 0);
    assert_link(base, "by-uuid", "e841ba0a-d7b9-42b6-b627-8ea27df85a54", "../../../sda");
    assert_link(base, "by-path", "pci-0000:02:00.1-ata-1.0", "../../../sda");
    assert_link(base, "by-diskseq", "2", "../../../sda");
    printf("test_disk_links apply-disk: OK\n");

    /* ---- apply: partition (suffix rule) ---- */
    char t2[] = "/tmp/schema-dl2-XXXXXX";
    char *root2 = mkdtemp(t2); assert(root2);
    char base2[512]; snprintf(base2, sizeof base2, "%s/disk", root2);

    struct uevent p; p.n = 0;
    put(&p, "SUBSYSTEM", "block");
    put(&p, "DEVNAME", "sda1");
    put(&p, "DEVTYPE", "partition");
    put(&p, "MAJOR", "8"); put(&p, "MINOR", "1");
    put(&p, "DISKSEQ", "2"); put(&p, "PARTN", "1");
    put(&p, "ID_PATH", "pci-0000:02:00.1-ata-1.0");
    put(&p, "ID_PART_ENTRY_UUID", "f746b242-7615-4bf6-9aca-b098677febc0");
    put(&p, "ID_PART_ENTRY_NAME", "Basic\\x20data\\x20partition");

    assert(disk_links_apply(base2, &p) == 0);
    /* suffixed */
    assert_link(base2, "by-path", "pci-0000:02:00.1-ata-1.0-part1", "../../../sda1");
    assert_link(base2, "by-diskseq", "2-part1", "../../../sda1");
    /* NOT suffixed, verbatim ENC name */
    assert_link(base2, "by-partuuid", "f746b242-7615-4bf6-9aca-b098677febc0", "../../../sda1");
    assert_link(base2, "by-partlabel", "Basic\\x20data\\x20partition", "../../../sda1");
    printf("test_disk_links apply-partition (suffix + ENC): OK\n");

    /* ---- gc: merge db record (derived props) + live ev (kernel props) ---- */
    char t3[] = "/tmp/schema-dldb-XXXXXX";
    char *dbdir = mkdtemp(t3); assert(dbdir);
    char rec[512]; snprintf(rec, sizeof rec, "%s/b8:1", dbdir);
    FILE *f = fopen(rec, "w"); assert(f);
    fprintf(f,
        "E:ID_PATH=pci-0000:02:00.1-ata-1.0\n"
        "E:ID_PART_ENTRY_UUID=f746b242-7615-4bf6-9aca-b098677febc0\n"
        "E:ID_PART_ENTRY_NAME=Basic\\x20data\\x20partition\n"
        "V:1\n");
    fclose(f);

    struct uevent rm; rm.n = 0;                 /* remove uevent: kernel props only */
    put(&rm, "SUBSYSTEM", "block");
    put(&rm, "MAJOR", "8"); put(&rm, "MINOR", "1");
    put(&rm, "DEVTYPE", "partition");
    put(&rm, "DISKSEQ", "2"); put(&rm, "PARTN", "1");

    assert(disk_links_gc(base2, dbdir, &rm) == 0);
    char chk[1024];
    snprintf(chk, sizeof chk, "%s/by-path/pci-0000:02:00.1-ata-1.0-part1", base2);
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-diskseq/2-part1", base2);   /* from grafted DISKSEQ+PARTN */
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-partuuid/f746b242-7615-4bf6-9aca-b098677febc0", base2);
    assert(access(chk, F_OK) != 0);
    snprintf(chk, sizeof chk, "%s/by-partlabel/Basic\\x20data\\x20partition", base2);
    assert(access(chk, F_OK) != 0);
    printf("test_disk_links gc (merge db+ev): OK\n");

    /* ---- wipe ---- */
    disk_links_wipe(base);
    assert(access(base, F_OK) != 0);
    printf("test_disk_links wipe: OK\n");

    printf("test_disk_links: ALL OK\n");
    return 0;
}
