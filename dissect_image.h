#ifndef DISSECT_IMAGE_H
#define DISSECT_IMAGE_H
#include "blkid_pt.h"   /* bpt_gpt_entry, bpt_guid_str, bpt_sector_size, pi_*, struct uevent, uevent_get/bpt_emit */
#include <string.h>
#include <strings.h>
#include <stdlib.h>

/* Discoverable Partitions Spec type-GUID -> designator. Arch-dependent
 * root/usr map only for the running arch (x86-64 on blakbox). Lowercase
 * canonical GUID strings, matching bpt_guid_str() output. */
struct dissect_map { const char *guid; const char *desig; };
static const struct dissect_map DISSECT_MAP[] = {
    { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "esp" },
    { "bc13c2ff-59e6-4262-a352-b275fd6f7172", "xbootldr" },
    { "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "swap" },
    { "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "home" },
    { "3b8f8425-20e0-4f3b-907f-1a25a76f98e8", "srv" },
    { "4d21b016-b534-45c2-a9fb-5c16e091fd2d", "var" },
    { "7ec6f557-3bc5-4aca-b293-16ef5df639d1", "tmp" },
    { "773f91ef-66d4-49b5-bd83-d683bf40ad16", "user-home" },
    /* x86-64 root */
    { "4f68bce3-e8cd-4db1-96e7-fbcaf984b709", "root" },
    { "2c7357ed-ebd2-46d9-aec1-23d437ec2bf5", "root-verity" },
    { "41092b05-9fc8-4523-994f-2def0408b176", "root-verity-sig" },
    /* x86-64 usr */
    { "8484680c-9521-48c6-9c11-b0720656f69e", "usr" },
    { "77ff5f63-e7b6-4633-acf4-1565b864c0e6", "usr-verity" },
    { "e7bb33fb-06cf-4e81-8273-e543b413e2e2", "usr-verity-sig" },
};

static inline const char *dissect_designator_for_guid(const char *type_guid_lc) {
    if (!type_guid_lc) return NULL;
    for (size_t i = 0; i < sizeof DISSECT_MAP / sizeof DISSECT_MAP[0]; i++)
        if (!strcasecmp(type_guid_lc, DISSECT_MAP[i].guid)) return DISSECT_MAP[i].desig;
    return NULL;
}
/* whole-disk probe: map each GPT partition's type GUID -> designator. */
static inline int dissect_probe_build(const char *sysroot, const char *devpath,
                                      const char *devnode, struct uevent *out) {
    if (!devnode) return 0;
    char syspath[PATH_MAX];
    if ((size_t)snprintf(syspath, sizeof syspath, "%s%s", sysroot ? sysroot : "",
                         devpath ? devpath : "") >= sizeof syspath) return 0;
    uint64_t ssz = bpt_sector_size(syspath);   /* 512 fallback for plain files */
    bpt_emit(out, "ID_DISSECT_IMAGE", "1");
    unsigned char ent[128];
    for (unsigned n = 1; n <= 128; n++) {
        if (bpt_gpt_entry(devnode, ssz, n, ent) != 0) break;   /* not GPT / past header */
        if (bpt_all_zero(ent, 16)) continue;                    /* unused slot */
        char g[37]; bpt_guid_str(ent + 0, g);
        const char *desig = dissect_designator_for_guid(g);
        if (!desig) continue;
        char key[48]; snprintf(key, sizeof key, "ID_DISSECT_PART%u_DESIGNATOR", n);
        bpt_emit(out, key, desig);
    }
    return 0;
}
#endif /* DISSECT_IMAGE_H */
