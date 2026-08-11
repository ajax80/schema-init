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

    printf("test_udev_executor: uevent_set OK\n");
    printf("test_udev_executor: ALL OK\n");
    return 0;
}
