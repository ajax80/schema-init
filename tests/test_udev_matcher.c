#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

int main(void) {
    /* alternation */
    assert(udev_glob("sd*|vd*", "sda") == 1);
    assert(udev_glob("sd*|vd*", "vdb") == 1);
    assert(udev_glob("sd*|vd*", "hda") == 0);
    /* globs delegated to fnmatch */
    assert(udev_glob("sd[a-c]", "sdb") == 1);
    assert(udev_glob("sd[a-c]", "sdd") == 0);
    assert(udev_glob("tty?", "ttyS") == 1);
    assert(udev_glob("event[0-9]", "event3") == 1);
    /* a '|' inside a bracket class is NOT an alternation split */
    assert(udev_glob("a[b|c]d", "abd") == 1);
    assert(udev_glob("a[b|c]d", "a|d") == 1);
    /* exact */
    assert(udev_glob("exact", "exact") == 1);
    assert(udev_glob("exact", "other") == 0);

    printf("test_udev_matcher: glob OK\n");
    return 0;
}
