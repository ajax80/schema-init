#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Build a raw kernel netlink buffer: "hdr\0K=V\0K=V\0..." Returns total length. */
static size_t mkbuf(char *dst, const char *hdr, const char **kv, int nkv) {
    size_t o = 0;
    memcpy(dst + o, hdr, strlen(hdr) + 1); o += strlen(hdr) + 1;
    for (int i = 0; i < nkv; i++) { memcpy(dst + o, kv[i], strlen(kv[i]) + 1); o += strlen(kv[i]) + 1; }
    return o;
}

int main(void) {
    char buf[4096];
    struct uevent ev;

    /* real captured usb_device add event */
    const char *kv1[] = {
        "ACTION=add", "DEVPATH=/devices/pci0000:00/usb1/1-4", "SUBSYSTEM=usb",
        "DEVNAME=/dev/bus/usb/001/002", "DEVTYPE=usb_device", "DRIVER=usb",
        "PRODUCT=a16f/304/620", "BUSNUM=001", "DEVNUM=002", "MAJOR=189", "MINOR=1"
    };
    size_t n1 = mkbuf(buf, "add@/devices/pci0000:00/usb1/1-4", kv1, 11);
    assert(uevent_parse(buf, n1, &ev) == 0);
    assert(ev.n == 11);
    assert(strcmp(uevent_get(&ev, "ACTION"), "add") == 0);
    assert(strcmp(uevent_get(&ev, "SUBSYSTEM"), "usb") == 0);
    assert(strcmp(uevent_get(&ev, "PRODUCT"), "a16f/304/620") == 0);
    assert(strcmp(uevent_get(&ev, "DEVNAME"), "/dev/bus/usb/001/002") == 0);
    assert(uevent_get(&ev, "NOPE") == NULL);

    /* remove event on a tty */
    const char *kv2[] = { "ACTION=remove", "SUBSYSTEM=tty", "DEVNAME=/dev/ttyUSB0" };
    size_t n2 = mkbuf(buf, "remove@/devices/x/ttyUSB0", kv2, 3);
    assert(uevent_parse(buf, n2, &ev) == 0);
    assert(strcmp(uevent_get(&ev, "ACTION"), "remove") == 0);

    /* malformed: no NUL at all -> not a kernel uevent */
    memcpy(buf, "garbage-no-nul", 14);
    assert(uevent_parse(buf, 14, &ev) == -1);

    /* malformed: header present but no ACTION key -> reject */
    const char *kv3[] = { "SUBSYSTEM=usb", "DEVPATH=/x" };
    size_t n3 = mkbuf(buf, "add@/x", kv3, 2);
    assert(uevent_parse(buf, n3, &ev) == -1);

    /* value containing '=' keeps everything after the first '=' */
    const char *kv4[] = { "ACTION=add", "MODALIAS=usb:v041Ep3272d0100" };
    size_t n4 = mkbuf(buf, "add@/x", kv4, 2);
    assert(uevent_parse(buf, n4, &ev) == 0);
    assert(strcmp(uevent_get(&ev, "MODALIAS"), "usb:v041Ep3272d0100") == 0);

    printf("test_uevent_parse: OK\n");
    return 0;
}
