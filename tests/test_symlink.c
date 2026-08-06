#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    struct dev_rule r; memset(&r, 0, sizeof r);

    /* Valid symlink name */
    assert(dev_rule_set(&r, "symlink", "esp32") == 0);
    assert(strcmp(r.symlink, "esp32") == 0);

    /* Valid with underscores and dashes */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "my_device-1") == 0);
    assert(strcmp(r.symlink, "my_device-1") == 0);

    /* Invalid: empty */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: contains slash */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "sub/esp32") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: dot-dot traversal */
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", "..") == -1);
    assert(r.symlink[0] == '\0');

    /* Invalid: 64 chars (exceeds max 63) */
    char longname[128]; memset(longname, 'a', 64); longname[64] = '\0';
    memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "symlink", longname) == -1);
    assert(r.symlink[0] == '\0');

    printf("test_symlink (grammar): OK\n");

    /* FS symlink apply/clear tests */
    char tmpl[] = "/tmp/schema-udev-test-symXXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base);

    char linkpath[256];
    snprintf(linkpath, sizeof linkpath, "%s/esp32", base);

    /* Create symlink: esp32 -> /dev/ttyUSB0 */
    assert(symlink_apply(base, "esp32", "ttyUSB0") == 0);

    char target[256];
    ssize_t len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/ttyUSB0") == 0);

    /* Atomic overwrite: change target to /dev/ttyUSB1 */
    assert(symlink_apply(base, "esp32", "ttyUSB1") == 0);
    len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/ttyUSB1") == 0);

    /* Absolute devname specified */
    assert(symlink_apply(base, "esp32", "/dev/bus/usb/001/002") == 0);
    len = readlink(linkpath, target, sizeof(target) - 1);
    assert(len > 0);
    target[len] = '\0';
    assert(strcmp(target, "/dev/bus/usb/001/002") == 0);

    /* Clear symlink */
    assert(symlink_clear(base, "esp32") == 0);
    assert(access(linkpath, F_OK) != 0);

    /* Clear non-existent symlink -> no error */
    assert(symlink_clear(base, "esp32") == 0);

    /* Clean up temp dir */
    rmdir(base);

    printf("test_symlink (FS apply/clear): OK\n");
    return 0;
}
