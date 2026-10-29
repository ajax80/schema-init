#include "input_id.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_mask(void) {
    uint64_t a[IID_NWORDS];

    /* single word: word0 = rightmost */
    iid_parse_mask("8000", a, 64);
    assert(iid_test_bit(a, 15, 64));           /* 0x8000 = bit 15 */
    assert(!iid_test_bit(a, 14, 64));

    /* multi-word: MSW first. "1f0000 0 0 0 0" -> word4 = 0x1f0000 */
    iid_parse_mask("1f0000 0 0 0 0", a, 64);
    assert(iid_test_bit(a, 4*64 + 16, 64));    /* BTN_LEFT = 0x110 = 272 */
    assert(iid_test_bit(a, 4*64 + 20, 64));    /* 0x114 = 276 */
    assert(!iid_test_bit(a, 4*64 + 21, 64));

    /* keyboard word0 mask */
    iid_parse_mask("fffffffffffffffe", a, 64);
    assert((a[0] & 0xFFFFFFFEULL) == 0xFFFFFFFEULL);
    assert(!iid_test_bit(a, 0, 64));
    assert(iid_test_bit(a, 1, 64));

    /* short field: high words zero, no OOB */
    iid_parse_mask("0", a, 64);
    assert(!iid_test_bit(a, 0, 64));
    assert(!iid_test_bit(a, 700, 64));

    /* NULL safe */
    iid_parse_mask(NULL, a, 64);
    assert(!iid_test_bit(a, 0, 64));

    /* any_bit range */
    iid_parse_mask("10000 0 0 0", a, 64);      /* word3 bit16 = 3*64+16 = 208 */
    assert(iid_any_bit(a, 200, 220, 64));
    assert(!iid_any_bit(a, 0, 200, 64));

    printf("test_mask OK\n");
}

/* 32-bit kernel: each sysfs word is 32 bits wide */
static void test_mask32(void) {
    uint64_t a[IID_NWORDS];

    iid_parse_mask("2 0", a, 32);                  /* word1 bit1 = bit 33 */
    assert(iid_test_bit(a, 33, 32));
    assert(!iid_test_bit(a, 1, 32));
    assert(!iid_test_bit(a, 32, 32));
    assert(!iid_test_bit(a, 63, 32));              /* word1 bit31, not a shift by 63 */

    iid_parse_mask("80000000 0", a, 32);            /* word1 bit31 = bit 63 */
    assert(iid_test_bit(a, 63, 32));
    assert(!iid_test_bit(a, 31, 32));

    /* BTN_LEFT (272) = word8 bit16 at 32 bits */
    iid_parse_mask("1f0000 0 0 0 0 0 0 0 0", a, 32);
    assert(iid_test_bit(a, 272, 32) && iid_test_bit(a, 276, 32));
    assert(!iid_test_bit(a, 277, 32));
    assert(iid_any_bit(a, BTN_MOUSE, BTN_JOYSTICK, 32));

    /* the same token at 64 bits is a different bit: width is not in the text */
    iid_parse_mask("1f0000 0 0 0 0", a, 64);
    assert(iid_test_bit(a, 272, 64));
    iid_parse_mask("1f0000 0 0 0 0", a, 32);
    assert(!iid_test_bit(a, 272, 32) && iid_test_bit(a, 4*32 + 16, 32));

    /* a 32-bit word never carries more than 32 bits */
    iid_parse_mask("100000000", a, 32);
    assert(!iid_any_bit(a, 0, 768, 32));

    iid_parse_mask("0", a, 32);
    assert(!iid_test_bit(a, 767, 32));
    assert(!iid_test_bit(a, 768, 32));             /* past IID_NWORDS: no OOB */

    printf("test_mask32 OK\n");
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

    uint64_t ev[IID_NWORDS], key[IID_NWORDS], rl[IID_NWORDS], ab[IID_NWORDS], pr[IID_NWORDS];
    assert(iid_read_masks(found, 64, ev, key, rl, ab, pr) == 0);
    assert(iid_test_bit(ev, 0, 64) && iid_test_bit(ev, 1, 64));   /* ev=3 */
    assert(iid_test_bit(key, 15, 64));                        /* 0x8000 word0 */
    assert(!iid_test_bit(pr, 0, 64));

    /* non-input path resolves to failure */
    char miss[PATH_MAX];
    assert(iid_find_input_node(root, "/devices", miss, sizeof miss) != 0);
    printf("test_discovery OK\n");
}

static int iid_has(const struct uevent *ev, const char *k) {
    const char *v = uevent_get(ev, k);
    return v && strcmp(v, "1") == 0;
}

static void test_heuristic(void) {
    uint64_t ev[IID_NWORDS], key[IID_NWORDS], rel[IID_NWORDS], ab[IID_NWORDS], pr[IID_NWORDS];
    struct uevent out;

    /* full keyboard: word0 = 0xfffffffffffffffe */
    iid_parse_mask("3", ev, 64); iid_parse_mask("fffffffffffffffe", key, 64);
    out.n = 0;
    assert(iid_test_key(ev, key, 64, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_KEYBOARD") && iid_has(&out, "ID_INPUT_KEY"));

    /* key-only: a media key in word3, not a full keyboard */
    iid_parse_mask("3", ev, 64); iid_parse_mask("100000 0 0 0", key, 64);
    out.n = 0;
    assert(iid_test_key(ev, key, 64, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_KEY") && !iid_has(&out, "ID_INPUT_KEYBOARD"));

    /* no EV_KEY -> nothing */
    iid_parse_mask("21", ev, 64); iid_parse_mask("0", key, 64);   /* ev = SYN|SW */
    out.n = 0;
    assert(iid_test_key(ev, key, 64, &out) == 0);
    assert(out.n == 0);

    /* mouse: EV_KEY|EV_REL, BTN_MOUSE range set, REL_X/Y set */
    iid_parse_mask("17", ev, 64);                     /* SYN|KEY|REL|MSC */
    iid_parse_mask("1f0000 0 0 0 0", key, 64);        /* BTN_LEFT.. word4 */
    iid_parse_mask("3", rel, 64);                     /* REL_X|REL_Y */
    iid_parse_mask("0", ab, 64); iid_parse_mask("0", pr, 64);
    out.n = 0;
    assert(iid_test_pointers(ev, ab, key, rel, pr, 64, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_MOUSE"));
    assert(!iid_has(&out, "ID_INPUT_TOUCHPAD") && !iid_has(&out, "ID_INPUT_TABLET"));

    /* accelerometer via prop bit 6 */
    iid_parse_mask("9", ev, 64); iid_parse_mask("0", key, 64);
    iid_parse_mask("0", rel, 64); iid_parse_mask("7", ab, 64); iid_parse_mask("40", pr, 64); /* prop bit6 */
    out.n = 0;
    assert(iid_test_pointers(ev, ab, key, rel, pr, 64, &out) == 1);
    assert(iid_has(&out, "ID_INPUT_ACCELEROMETER"));

    /* touchpad: ABS_X/Y + BTN_TOOL_FINGER, no pen, not direct */
    iid_parse_mask("9", ev, 64);                                  /* SYN|ABS */
    { uint64_t k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_TOOL_FINGER/64] |= 1ULL << (BTN_TOOL_FINGER%64);
      iid_parse_mask("3", ab, 64);                                /* ABS_X|ABS_Y */
      iid_parse_mask("0", rel, 64); iid_parse_mask("0", pr, 64);
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, 64, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_TOUCHPAD"));
      assert(!iid_has(&out, "ID_INPUT_MOUSE")); }

    /* touchscreen: MT coords + INPUT_PROP_DIRECT */
    { uint64_t a[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) a[i]=0;
      a[ABS_MT_POSITION_X/64] |= 1ULL << (ABS_MT_POSITION_X%64);
      a[ABS_MT_POSITION_Y/64] |= 1ULL << (ABS_MT_POSITION_Y%64);
      iid_parse_mask("9", ev, 64); iid_parse_mask("0", key, 64);
      iid_parse_mask("0", rel, 64); iid_parse_mask("2", pr, 64);      /* prop bit1 = DIRECT */
      out.n = 0;
      assert(iid_test_pointers(ev, a, key, rel, pr, 64, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_TOUCHSCREEN")); }

    /* joystick: BTN_JOYSTICK button */
    { uint64_t k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_JOYSTICK/64] |= 1ULL << (BTN_JOYSTICK%64);
      iid_parse_mask("3", ev, 64); iid_parse_mask("0", rel, 64);
      iid_parse_mask("0", ab, 64); iid_parse_mask("0", pr, 64);
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, 64, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_JOYSTICK")); }

    /* pointingstick: prop bit5 + mouse buttons */
    { uint64_t k[IID_NWORDS]; for (int i=0;i<IID_NWORDS;i++) k[i]=0;
      k[BTN_MOUSE/64] |= 1ULL << (BTN_MOUSE%64);
      iid_parse_mask("17", ev, 64); iid_parse_mask("3", rel, 64);
      iid_parse_mask("0", ab, 64); iid_parse_mask("20", pr, 64);      /* prop bit5 */
      out.n = 0;
      assert(iid_test_pointers(ev, ab, k, rel, pr, 64, &out) == 1);
      assert(iid_has(&out, "ID_INPUT_POINTINGSTICK") && iid_has(&out, "ID_INPUT_MOUSE")); }

    printf("test_heuristic OK\n");
}

static void test_build_full(void) {
    char root[] = "/tmp/iidbuildXXXXXX";
    assert(mkdtemp(root));

    /* keyboard node */
    iid_make_node(root, "/devices/kbd", "120013", "fffffffffffffffe", "0", "0", "0"); /* word0 = keyboard mask */
    char ev0[PATH_MAX]; snprintf(ev0, sizeof ev0, "%s/devices/kbd/event0", root); iid_mkdirs(ev0);
    struct uevent e; assert(input_id_build(root, "/devices/kbd/event0", 64, &e) == 0);
    assert(iid_has(&e, "ID_INPUT"));
    assert(iid_has(&e, "ID_INPUT_KEYBOARD") && iid_has(&e, "ID_INPUT_KEY"));
    assert(!iid_has(&e, "ID_INPUT_MOUSE"));

    /* switch-only node (audio jack): ev = SYN|SW, sw set */
    iid_make_node(root, "/devices/sw", "21", "0", "0", "0", "0");
    assert(input_id_build(root, "/devices/sw", 64, &e) == 0);
    assert(iid_has(&e, "ID_INPUT") && iid_has(&e, "ID_INPUT_SWITCH"));
    assert(!iid_has(&e, "ID_INPUT_KEY"));

    /* scrollwheel-only: EV_REL + REL_WHEEL, no buttons -> ID_INPUT_KEY + ID_INPUT */
    iid_make_node(root, "/devices/whl", "4", "0", "100", "0", "0");   /* ev = EV_REL only; rel bit8 = REL_WHEEL */
    assert(input_id_build(root, "/devices/whl", 64, &e) == 0);
    assert(iid_has(&e, "ID_INPUT") && iid_has(&e, "ID_INPUT_KEY"));

    /* not an input device */
    assert(input_id_build(root, "/devices", 64, &e) != 0);

    /* 32-bit kernel mouse: BTN_LEFT in word8, buttons only below it */
    iid_make_node(root, "/devices/m32", "7", "1f0000 0 0 0 0 0 0 0 0", "3", "0", "0");
    assert(input_id_build(root, "/devices/m32", 32, &e) == 0);
    assert(iid_has(&e, "ID_INPUT") && iid_has(&e, "ID_INPUT_MOUSE"));
    assert(!iid_has(&e, "ID_INPUT_KEY"));

    /* 32-bit kernel keyboard: word0 = fffffffe */
    iid_make_node(root, "/devices/k32", "120013", "fffffffe", "0", "0", "0");
    assert(input_id_build(root, "/devices/k32", 32, &e) == 0);
    assert(iid_has(&e, "ID_INPUT_KEYBOARD") && iid_has(&e, "ID_INPUT_KEY"));

    printf("test_build_full OK\n");
}

int main(void) {
    test_mask();
    test_mask32();
    test_discovery();
    test_heuristic();
    test_build_full();
    printf("ALL input_id tests passed\n");
    return 0;
}
