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

/* blscfg's menu: by the version key, newest first, then file name; ents[0] boots when saved_entry is unset */
static void test_sort(void) {
    static const char *const in[][2] = {
        { "c8d9-6.1.0", "6.1.0" }, { "c8d9-7.2.9", "7.2.9" }, { "c8d9-0-rescue", "0-rescue-c8d9" },
        { "c8d9-7.2.11", "7.2.11" }, { "c8d9-6.10.1", "6.10.1" }, { "c8d9-7.2.10", "7.2.10" },
        /* the file name sorts it first, its version doesn't */
        { "schema-snap-20261118-175630-post-bootfix", "7.2.11" }, { "c8d9-7.3.0", "7.3.0" },
        { "zz-noversion", "" }, { "aa-noversion", "" },
    };
    static const char *const want[] = { "c8d9-7.3.0", "schema-snap-20261118-175630-post-bootfix", "c8d9-7.2.11",
                                        "c8d9-7.2.10", "c8d9-7.2.9", "c8d9-6.10.1", "c8d9-6.1.0", "c8d9-0-rescue",
                                        "zz-noversion", "aa-noversion" };
    enum { N = sizeof in / sizeof *in };
    bls_entry_t e[N];
    memset(e, 0, sizeof e);
    for (int i = 0; i < N; i++) {
        snprintf(e[i].id, sizeof e[i].id, "%s", in[i][0]);
        snprintf(e[i].version, sizeof e[i].version, "%s", in[i][1]);
    }
    bls_sort(e, N);
    for (int i = 0; i < N; i++) assert(!strcmp(e[i].id, want[i]));
}

int main(void) {
    test_vercmp();
    test_sort();
    puts("test_verify: ok");
    return 0;
}
