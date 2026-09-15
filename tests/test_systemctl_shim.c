#include "../systemctl_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_strip_suffix(void) {
    char b[256];
    assert(strcmp(strip_service_suffix("foo.service", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo.socket", b, sizeof b), "foo.socket") == 0);
}

static void test_supported(void) {
    assert(unit_supported("foo.service") == 1);
    assert(unit_supported("foo") == 1);
    assert(unit_supported("foo@bar.service") == 0);
    assert(unit_supported("getty@tty1.service") == 0);
    assert(unit_supported("foo.socket") == 0);
    assert(unit_supported("foo.timer") == 0);
    assert(unit_supported("foo.path") == 0);
    assert(unit_supported("foo.target") == 0);
    assert(unit_supported("foo.mount") == 0);
    assert(unit_supported("foo.slice") == 0);
    assert(unit_supported("foo.scope") == 0);
}

int main(void) {
    test_strip_suffix();
    test_supported();
    printf("task1 systemctl-shim tests passed\n");
    return 0;
}
