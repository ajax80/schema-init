#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add");
    ue_set(&ev, "DEVPATH", "/devices/x");
    ue_set(&ev, "FOO", "one");

    /* overwrite existing */
    assert(uevent_set(&ev, "FOO", "two") == 0);
    assert(strcmp(uevent_get(&ev, "FOO"), "two") == 0);
    /* append new */
    assert(uevent_set(&ev, "BAR", "baz") == 0);
    assert(strcmp(uevent_get(&ev, "BAR"), "baz") == 0);

    /* a later rule's ENV== sees the updated value */
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(ctx.nsym == 0 && ctx.link_priority == 0 && ctx.escape == 0);
    assert(ctx.nfinal == 0 && ctx.deferred_applies == 0);
    struct rule r;
    ruleset_parse_line("ENV{FOO}==\"two\"", &r);
    assert(rule_match(&r, &ctx) == 1);

    struct dev_ctx tc; memset(&tc, 0, sizeof tc);
    ctx_add_tag(&tc, "systemd");
    ctx_add_tag(&tc, "systemd");        /* dedupe */
    ctx_add_tag(&tc, "seat");
    assert(tc.ntags == 2);
    ctx_del_tag(&tc, "systemd");
    assert(tc.ntags == 1 && strcmp(tc.tags[0], "seat") == 0);
    ctx_del_tag(&tc, "nope");           /* absent: no-op */
    assert(tc.ntags == 1);
    ctx_clear_tags(&tc);
    assert(tc.ntags == 0);
    printf("test_udev_executor: tag-ops OK\n");

    char esc[64];
    udev_replace_chars("a b/c", esc, sizeof esc);   /* space -> _, '/' kept */
    assert(strcmp(esc, "a_b/c") == 0);
    udev_replace_chars("wwn-0x5!bad", esc, sizeof esc); /* '!' -> _ */
    assert(strcmp(esc, "wwn-0x5_bad") == 0);

    struct dev_ctx sc; memset(&sc, 0, sizeof sc);
    ctx_add_symlink(&sc, "disk/by-id/a");
    ctx_add_symlink(&sc, "disk/by-id/a");           /* dedupe */
    ctx_add_symlink(&sc, "disk/by-path/b");
    ctx_add_symlink(&sc, "");                        /* empty: no-op */
    assert(sc.nsym == 2);
    ctx_del_symlink(&sc, "disk/by-id/a");
    assert(sc.nsym == 1 && strcmp(sc.symlinks[0], "disk/by-path/b") == 0);
    ctx_clear_symlinks(&sc);
    assert(sc.nsym == 0);
    printf("test_udev_executor: symlink-ops OK\n");

    struct dev_ctx oc; memset(&oc, 0, sizeof oc);
    apply_options(&oc, "link_priority=10");
    assert(oc.link_priority == 10);
    apply_options(&oc, "string_escape=replace");
    assert(oc.escape == 1);
    apply_options(&oc, "string_escape=none");
    assert(oc.escape == 0);
    apply_options(&oc, "static_node=foo");           /* no-op, no crash */
    apply_options(&oc, "db_persist, link_priority=-5"); /* mixed list */
    assert(oc.link_priority == -5);

    struct rule_clause c1; memset(&c1, 0, sizeof c1);
    safe_copy(c1.key, "NAME", sizeof c1.key);
    assert(ctx_key_final(&oc, &c1) == 0);
    ctx_lock_final(&oc, &c1);
    assert(ctx_key_final(&oc, &c1) == 1);
    ctx_lock_final(&oc, &c1);                          /* idempotent */
    assert(oc.nfinal == 1);
    struct rule_clause c2; memset(&c2, 0, sizeof c2);
    safe_copy(c2.key, "ENV", sizeof c2.key);
    safe_copy(c2.subkey, "FOO", sizeof c2.subkey);
    assert(ctx_key_final(&oc, &c2) == 0);              /* ENV{FOO} distinct from NAME */
    printf("test_udev_executor: options+final OK\n");

    printf("test_udev_executor: uevent_set OK\n");

    /* build a dev_ctx over a minimal device */
    struct uevent ae; memset(&ae, 0, sizeof ae);
    ue_set(&ae, "ACTION", "add"); ue_set(&ae, "DEVPATH", "/devices/z");
    struct dev_ctx ac; assert(dev_ctx_init(&ac, &ae, "/sys") == 0);

    struct rule ar;
    ruleset_parse_line("ENV{MYK}=\"v1\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"", &ar);
    assert(apply_rule(&ar, &ac) == NULL);
    assert(strcmp(uevent_get(&ae, "MYK"), "v1") == 0);
    assert(ac.ntags == 1 && strcmp(ac.tags[0], "uaccess") == 0);
    assert(strcmp(ac.mode, "0660") == 0 && strcmp(ac.group, "plugdev") == 0);

    /* SYMLINK+= with a space-separated list -> two links */
    ruleset_parse_line("SYMLINK+=\"disk/by-id/x disk/by-path/y\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.nsym == 2);

    /* string_escape=replace: whitespace escaped -> a single link */
    ac.escape = 1;
    ruleset_parse_line("SYMLINK+=\"has space\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.nsym == 3 && strcmp(ac.symlinks[2], "has_space") == 0);
    ac.escape = 0;

    /* TAG-= removes */
    ruleset_parse_line("TAG-=\"uaccess\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.ntags == 0);

    /* := locks the key: a later = does not override */
    ruleset_parse_line("NAME:=\"locked\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(ac.name, "locked") == 0);
    ruleset_parse_line("NAME=\"other\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(ac.name, "locked") == 0);

    /* GOTO returns the target label */
    ruleset_parse_line("GOTO=\"end_here\"", &ar);
    const char *g = apply_rule(&ar, &ac);
    assert(g != NULL && strcmp(g, "end_here") == 0);

    /* substitution runs on values */
    ruleset_parse_line("ENV{KN}=\"%k\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(uevent_get(&ae, "KN"), "z") == 0);
    printf("test_udev_executor: apply-rule OK\n");

    /* helper: append a parsed line to a ruleset */
    #define ADD(RS, LINE) do { struct rule _r; \
        assert(ruleset_parse_line((LINE), &_r) > 0); \
        assert(ruleset_append((RS), &_r) == 0); } while (0)

    /* ENV set by an early rule is visible to a later rule's ENV== match */
    struct uevent de; memset(&de, 0, sizeof de);
    ue_set(&de, "ACTION", "add"); ue_set(&de, "DEVPATH", "/devices/w");
    struct dev_ctx dc; assert(dev_ctx_init(&dc, &de, "/sys") == 0);
    struct ruleset rs1 = {0};
    ADD(&rs1, "ACTION==\"add\", ENV{PHASE}=\"two\"");
    ADD(&rs1, "ENV{PHASE}==\"two\", TAG+=\"reached\"");
    assert(ruleset_apply(&rs1, &dc) == 0);
    assert(dc.ntags == 1 && strcmp(dc.tags[0], "reached") == 0);
    free(rs1.rules);

    /* GOTO skips the intervening rule's assignment */
    struct dev_ctx gc; memset(&gc, 0, sizeof gc);
    struct uevent ge; memset(&ge, 0, sizeof ge);
    ue_set(&ge, "ACTION", "add"); ue_set(&ge, "DEVPATH", "/devices/g");
    assert(dev_ctx_init(&gc, &ge, "/sys") == 0);
    struct ruleset rs2 = {0};
    ADD(&rs2, "ACTION==\"add\", GOTO=\"skip\"");
    ADD(&rs2, "TAG+=\"should_not_appear\"");
    ADD(&rs2, "LABEL=\"skip\"");
    ADD(&rs2, "TAG+=\"after_label\"");
    assert(ruleset_apply(&rs2, &gc) == 0);
    assert(gc.ntags == 1 && strcmp(gc.tags[0], "after_label") == 0);
    free(rs2.rules);

    /* TEST now gates natively (R4a): a nonexistent path no longer applies at all */
    struct dev_ctx fc; memset(&fc, 0, sizeof fc);
    struct uevent fe; memset(&fe, 0, sizeof fe);
    ue_set(&fe, "ACTION", "add"); ue_set(&fe, "DEVPATH", "/devices/f");
    assert(dev_ctx_init(&fc, &fe, "/sys") == 0);
    struct ruleset rs3 = {0};
    ADD(&rs3, "ACTION==\"add\", TEST==\"/nonexistent/path\", TAG+=\"superset\"");
    assert(ruleset_apply(&rs3, &fc) == 0);
    assert(fc.ntags == 0);
    assert(fc.deferred_applies == 0);
    free(rs3.rules);

    /* TEST resolving true applies the rule without inflating the deferred counter */
    struct dev_ctx fc2; memset(&fc2, 0, sizeof fc2);
    struct uevent fe2; memset(&fe2, 0, sizeof fe2);
    ue_set(&fe2, "ACTION", "add"); ue_set(&fe2, "DEVPATH", "/devices/f2");
    assert(dev_ctx_init(&fc2, &fe2, "/sys") == 0);
    struct ruleset rs3b = {0};
    ADD(&rs3b, "ACTION==\"add\", TEST==\"/\", TAG+=\"present\"");
    assert(ruleset_apply(&rs3b, &fc2) == 0);
    assert(fc2.ntags == 1 && strcmp(fc2.tags[0], "present") == 0);
    assert(fc2.deferred_applies == 0);
    free(rs3b.rules);

    /* PROGRAM now gates natively (R4b): a nonexistent helper no longer applies at all */
    struct dev_ctx pc; memset(&pc, 0, sizeof pc);
    struct uevent pe; memset(&pe, 0, sizeof pe);
    ue_set(&pe, "ACTION", "add"); ue_set(&pe, "DEVPATH", "/devices/p");
    ue_set(&pe, "SUBSYSTEM", "drm");
    assert(dev_ctx_init(&pc, &pe, "/sys") == 0);
    struct ruleset rs5 = {0};
    ADD(&rs5, "SUBSYSTEM==\"drm\", PROGRAM=\"/nonexistent/helper\", TAG+=\"prog_gate\"");
    assert(ruleset_apply(&rs5, &pc) == 0);
    assert(pc.ntags == 0);
    free(rs5.rules);

    /* GOTO to a missing label stops cleanly (no crash, no later apply) */
    struct dev_ctx mc; memset(&mc, 0, sizeof mc);
    struct uevent me; memset(&me, 0, sizeof me);
    ue_set(&me, "ACTION", "add"); ue_set(&me, "DEVPATH", "/devices/m");
    assert(dev_ctx_init(&mc, &me, "/sys") == 0);
    struct ruleset rs4 = {0};
    ADD(&rs4, "GOTO=\"nowhere\"");
    ADD(&rs4, "TAG+=\"unreached\"");
    assert(ruleset_apply(&rs4, &mc) == 0);
    assert(mc.ntags == 0);
    free(rs4.rules);
    printf("test_udev_executor: driver OK\n");

    /* live smoke: apply the whole installed ruleset to a real device, no crash */
    if (access("/sys/block/sda", F_OK) == 0) {
        char lnk[PATH_MAX]; ssize_t ln = readlink("/sys/block/sda", lnk, sizeof lnk - 1);
        assert(ln > 0); lnk[ln] = '\0';
        const char *dp = strstr(lnk, "/devices/"); assert(dp);
        struct uevent le; memset(&le, 0, sizeof le);
        ue_set(&le, "ACTION", "add"); ue_set(&le, "DEVPATH", dp);
        ue_set(&le, "SUBSYSTEM", "block");
        struct dev_ctx lc; assert(dev_ctx_init(&lc, &le, "/sys") == 0);
        const char *real[] = { "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };
        struct ruleset live = {0};
        assert(ruleset_load_dirs(real, 3, &live) == 0);
        assert(ruleset_apply(&live, &lc) == 0);   /* must not crash */
        assert(lc.ntags >= 0);                    /* sda typically gets "systemd" */
        free(live.rules);
        printf("test_udev_executor: live-smoke OK (sda tags=%d symlinks=%d)\n", lc.ntags, lc.nsym);
    }
    #undef ADD
    printf("test_udev_executor: ALL OK\n");
    return 0;
}
