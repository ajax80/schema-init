#include "../schema-udev.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>

static void put(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

static uint32_t rd32(const char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(void) {
    /* 1. murmur2 ground-truth vectors (captured from real udev) */
    assert(murmur2("mem") == 0xc365cd83u);
    assert(murmur2("tty") == 0x8afa90c8u);

    /* 2. Build a frame for a synthetic mem/change event */
    struct uevent ev; memset(&ev, 0, sizeof ev);
    put(&ev, "ACTION", "change");
    put(&ev, "DEVPATH", "/devices/virtual/mem/null");
    put(&ev, "SUBSYSTEM", "mem");
    put(&ev, "DEVNAME", "/dev/null");
    put(&ev, "MAJOR", "1");
    put(&ev, "MINOR", "3");

    char buf[4096];
    ssize_t n = libudev_frame_build(&ev, buf, sizeof buf);
    assert(n > 40);

    assert(memcmp(buf, "libudev", 7) == 0 && buf[7] == '\0');
    assert(be32toh(rd32(buf + 8)) == 0xfeedcafeu);   /* magic: big-endian */
    assert(rd32(buf + 12) == 40);                    /* header_size: native */
    assert(rd32(buf + 16) == 40);                    /* properties_off: native */
    uint32_t plen = rd32(buf + 20);                  /* properties_len: native */
    assert((ssize_t)(40 + plen) == n);
    assert(be32toh(rd32(buf + 24)) == 0xc365cd83u);  /* subsystem hash: big-endian */
    assert(rd32(buf + 28) == 0);                     /* no DEVTYPE -> 0 */
    assert(rd32(buf + 32) == 0 && rd32(buf + 36) == 0); /* bloom hi/lo */

    /* 3. Self-decode round-trip: parse the properties back the libudev way */
    struct uevent back; memset(&back, 0, sizeof back);
    uint32_t off = rd32(buf + 16);
    uint32_t i = off;
    while (i < (uint32_t)n && back.n < UE_MAX_KEYS) {
        const char *rec = buf + i;
        size_t rl = strlen(rec);
        if (rl == 0) { i++; continue; }
        const char *eq = strchr(rec, '=');
        if (eq) {
            size_t kl = (size_t)(eq - rec);
            safe_copy(back.key[back.n], rec, kl + 1);
            safe_copy(back.val[back.n], eq + 1, UE_VAL_MAX);
            back.n++;
        }
        i += rl + 1;
    }
    assert(strcmp(uevent_get(&back, "ACTION"), "change") == 0);
    assert(strcmp(uevent_get(&back, "SUBSYSTEM"), "mem") == 0);
    assert(strcmp(uevent_get(&back, "DEVNAME"), "/dev/null") == 0);

    /* 4. The committed golden frame decodes to the documented header */
    FILE *f = fopen("tests/fixtures/libudev-frame-mem-change.bin", "rb");
    assert(f);
    char gold[8192];
    size_t glen = fread(gold, 1, sizeof gold, f);
    fclose(f);
    assert(glen >= 40);
    assert(memcmp(gold, "libudev", 7) == 0);
    assert(be32toh(rd32(gold + 8)) == 0xfeedcafeu);
    assert(rd32(gold + 12) == 40);
    assert(rd32(gold + 16) == 40);
    assert(be32toh(rd32(gold + 24)) == 0xc365cd83u);

    /* 5. Missing required key -> -1; tiny buffer -> -1 */
    struct uevent bad; memset(&bad, 0, sizeof bad);
    put(&bad, "ACTION", "add");   /* no DEVPATH/SUBSYSTEM */
    assert(libudev_frame_build(&bad, buf, sizeof buf) == -1);
    assert(libudev_frame_build(&ev, buf, 8) == -1);

    printf("test_libudev_frame: OK\n");
    return 0;
}
