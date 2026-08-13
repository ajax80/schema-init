#include "../udev_ruleset.h"
#include "../udev_builtins.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

static void test_result_subst(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(ctx.result[0] == '\0');
    safe_copy(ctx.result, "alpha beta gamma", sizeof ctx.result);
    char out[UE_VAL_MAX];
    ruleset_subst("$result", &ctx, out, sizeof out);   assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c", &ctx, out, sizeof out);         assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c{2}", &ctx, out, sizeof out);      assert(!strcmp(out, "beta"));
    ruleset_subst("$result{3}", &ctx, out, sizeof out); assert(!strcmp(out, "gamma"));
    ruleset_subst("%c{9}", &ctx, out, sizeof out);      assert(!strcmp(out, ""));
    printf("test_udev_r4b: result-subst OK\n");
}

int main(void) {
    test_result_subst();
    printf("test_udev_r4b: ALL OK\n");
    return 0;
}
