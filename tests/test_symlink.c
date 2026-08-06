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
    return 0;
}
