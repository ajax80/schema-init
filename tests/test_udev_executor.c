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
    printf("test_udev_executor: ALL OK\n");
    return 0;
}
