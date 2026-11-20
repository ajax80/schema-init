#include "../verify.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_vercmp(void) {
    static const char *const v[][2] = {
        { "m-7.2.10", "m-7.2.11" }, { "m-6.1", "m-6.10" }, { "m-7.2.9", "m-7.2.10" },
        { "c8d9-7.0.12-201.fc44", "c8d9-7.2.11-cachyos1" }, { "m-0-rescue", "m-7.2.11" },
        { "m-7.2", "m-7.2.1" }, { "a", "b" },
    };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        assert(bls_vercmp(v[i][0], v[i][1]) < 0);
        assert(bls_vercmp(v[i][1], v[i][0]) > 0);
    }
    assert(bls_vercmp("m-7.2.11", "m-7.2.11") == 0);
    assert(bls_vercmp("m-007", "m-7") == 0);
}

/* blscfg's menu: newest kernel first, which is ents[0] when saved_entry is unset */
static void test_sort(void) {
    static const char *const in[] = { "m-6.1.0", "m-7.2.9", "m-0-rescue", "m-7.2.11", "m-6.10.1", "m-7.2.10" };
    static const char *const want[] = { "m-7.2.11", "m-7.2.10", "m-7.2.9", "m-6.10.1", "m-6.1.0", "m-0-rescue" };
    bls_entry_t e[6];
    memset(e, 0, sizeof e);
    for (int i = 0; i < 6; i++) snprintf(e[i].id, sizeof e[i].id, "%s", in[i]);
    bls_sort(e, 6);
    for (int i = 0; i < 6; i++) assert(!strcmp(e[i].id, want[i]));
}

int main(void) {
    test_vercmp();
    test_sort();
    puts("test_verify: ok");
    return 0;
}
