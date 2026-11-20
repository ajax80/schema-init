#include "../verify.h"
#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
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

/* F44 blscfg (grub2-2.12-60): bls_cmp compares the file names (".conf" stripped) by
 * split_cmp: name, version, release split at the last two '-', each by rpm's vercmp;
 * newest first. With saved_entry unset GRUB boots ents[0]. */
static void sorted(const char *const *in, const char *const *want, int n) {
    bls_entry_t *e = calloc((size_t)n, sizeof *e);
    for (int i = 0; i < n; i++) snprintf(e[i].id, sizeof e[i].id, "%s", in[i]);
    bls_sort(e, n);
    for (int i = 0; i < n; i++) {
        if (strcmp(e[i].id, want[i])) fprintf(stderr, "sort[%d]: %s, want %s\n", i, e[i].id, want[i]);
        assert(!strcmp(e[i].id, want[i]));
    }
    free(e);
}

static void test_sort(void) {
    /* the name part decides first: a snapshot sorts above every machine-id kernel, and a
     * new kernel next to it does not change that; a release with a '-' in it moves the
     * kernel version into the name, so 7.2.11-cachyos1-schema outranks a stock 7.3.0-200 */
    static const char *const in[] = { "c8d9-7.3.0-200.fc44.x86_64", "schema-snap-20261118-175630-post-bootfix",
                                      "c8d9-7.2.11-cachyos1-schema.fc44.x86_64", "c8d9-0-rescue" };
    static const char *const want[] = { "schema-snap-20261118-175630-post-bootfix",
                                        "c8d9-7.2.11-cachyos1-schema.fc44.x86_64", "c8d9-7.3.0-200.fc44.x86_64",
                                        "c8d9-0-rescue" };
    sorted(in, want, 4);
    /* one '-': name and version; none: name only */
    static const char *const in2[] = { "a", "a-1", "a-2", "a-10" };
    static const char *const want2[] = { "a-10", "a-2", "a-1", "a" };
    sorted(in2, want2, 4);
}

/* vercmp's tilde (before everything) and plus (after the base version) */
static void test_vercmp_sep(void) {
    assert(bls_vercmp("1.0~rc1", "1.0") < 0);
    assert(bls_vercmp("1.0~rc1", "1.0~rc2") < 0);
    assert(bls_vercmp("1.0+p1", "1.0") > 0);
    assert(bls_vercmp("1.0", "1.0+p1") < 0);
    assert(bls_vercmp("1.0+p1", "1.0.1") < 0);
}

/* blakbox's real entry set and the order this GRUB lists it in */
static void test_sort_blakbox(void) {
    static const char *const want[] = {
        "schema-snap-20261118-175630-post-bootfix",
        "schema-snap-20261011-200415-post-update",
        "c8d9417838064f29b0660ea971162a87-7.2.11-cachyos1-schema.fc44.x86_64",
        "c8d9417838064f29b0660ea971162a87-7.0.12-cachyos1-schema.fc44.x86_64",
        "c8d9417838064f29b0660ea971162a87-7.0.12-201.fc44.x86_64",
        "c8d9417838064f29b0660ea971162a87-0-rescue",
    };
    enum { N = sizeof want / sizeof *want };
    const char *dir = "tests/fixtures/verify/boot/blakbox-entries";
    struct dirent **d;
    int nd = scandir(dir, &d, NULL, alphasort), n = 0;
    bls_entry_t e[N];
    char p[1024];
    for (int i = 0; i < nd; i++) {
        size_t l = strlen(d[i]->d_name);
        if (l > 5 && !strcmp(d[i]->d_name + l - 5, ".conf")) {
            snprintf(p, sizeof p, "%s/%s", dir, d[i]->d_name);
            assert(n < N && bls_parse(p, &e[n++]) == 0);
        }
        free(d[i]);
    }
    free(d);
    assert(n == N);
    bls_sort(e, N);
    for (int i = 0; i < N; i++) assert(!strcmp(e[i].id, want[i]));
}

int main(void) {
    test_vercmp();
    test_sort();
    test_vercmp_sep();
    test_sort_blakbox();
    puts("test_verify: ok");
    return 0;
}
