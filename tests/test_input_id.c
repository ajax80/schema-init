#include "input_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mask(void) {
    unsigned long a[IID_NWORDS];

    /* single word: word0 = rightmost */
    iid_parse_mask("8000", a);
    assert(iid_test_bit(a, 15));           /* 0x8000 = bit 15 */
    assert(!iid_test_bit(a, 14));

    /* multi-word: MSW first. "1f0000 0 0 0 0" -> word4 = 0x1f0000 */
    iid_parse_mask("1f0000 0 0 0 0", a);
    assert(iid_test_bit(a, 4*64 + 16));    /* BTN_LEFT = 0x110 = 272 */
    assert(iid_test_bit(a, 4*64 + 20));    /* 0x114 = 276 */
    assert(!iid_test_bit(a, 4*64 + 21));

    /* keyboard word0 mask */
    iid_parse_mask("fffffffffffffffe", a);
    assert((a[0] & 0xFFFFFFFEUL) == 0xFFFFFFFEUL);
    assert(!iid_test_bit(a, 0));
    assert(iid_test_bit(a, 1));

    /* short field: high words zero, no OOB */
    iid_parse_mask("0", a);
    assert(!iid_test_bit(a, 0));
    assert(!iid_test_bit(a, 700));

    /* NULL safe */
    iid_parse_mask(NULL, a);
    assert(!iid_test_bit(a, 0));

    /* any_bit range */
    iid_parse_mask("10000 0 0 0", a);      /* word3 bit16 = 3*64+16 = 208 */
    assert(iid_any_bit(a, 200, 220));
    assert(!iid_any_bit(a, 0, 200));

    printf("test_mask OK\n");
}

#include <sys/stat.h>
#include <limits.h>

static void iid_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static void iid_wf(const char *dir, const char *name, const char *val) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(val, f); fputc('\n', f); fclose(f);
}

/* build <root><rel>/{capabilities/{ev,key,rel,abs},properties}; returns dir written into caps parent */
static void iid_make_node(const char *root, const char *rel,
                          const char *ev, const char *key, const char *rel_, const char *abs_, const char *prop) {
    char node[PATH_MAX], caps[PATH_MAX];
    snprintf(node, sizeof node, "%s%s", root, rel);
    if ((size_t)snprintf(caps, sizeof caps, "%s/capabilities", node) >= sizeof caps) assert(0);
    iid_mkdirs(caps);
    iid_wf(caps, "ev", ev); iid_wf(caps, "key", key);
    iid_wf(caps, "rel", rel_); iid_wf(caps, "abs", abs_);
    iid_wf(node, "properties", prop);
}

static void test_discovery(void) {
    char root[] = "/tmp/iidtestXXXXXX";
    assert(mkdtemp(root));
    iid_make_node(root, "/devices/inp0", "3", "8000", "0", "0", "0");   /* word0 = 0x8000 */
    /* also create an eventN child dir to resolve up from */
    char evdir[PATH_MAX]; snprintf(evdir, sizeof evdir, "%s/devices/inp0/event0", root);
    iid_mkdirs(evdir);

    char found[PATH_MAX];
    assert(iid_find_input_node(root, "/devices/inp0/event0", found, sizeof found) == 0);
    char want[PATH_MAX]; snprintf(want, sizeof want, "%s/devices/inp0", root);
    assert(strcmp(found, want) == 0);

    unsigned long ev[IID_NWORDS], key[IID_NWORDS], rl[IID_NWORDS], ab[IID_NWORDS], pr[IID_NWORDS];
    assert(iid_read_masks(found, ev, key, rl, ab, pr) == 0);
    assert(iid_test_bit(ev, 0) && iid_test_bit(ev, 1));   /* ev=3 */
    assert(iid_test_bit(key, 15));                        /* 0x8000 word0 */
    assert(!iid_test_bit(pr, 0));

    /* non-input path resolves to failure */
    char miss[PATH_MAX];
    assert(iid_find_input_node(root, "/devices", miss, sizeof miss) != 0);
    printf("test_discovery OK\n");
}

int main(void) {
    test_mask();
    test_discovery();
    printf("ALL input_id tests passed\n");
    return 0;
}
