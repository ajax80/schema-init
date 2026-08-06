#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void set(struct uevent *ev, const char *k, const char *v) {
    snprintf(ev->key[ev->n], UE_KEY_MAX, "%s", k);
    snprintf(ev->val[ev->n], UE_VAL_MAX, "%s", v);
    ev->n++;
}

int main(void) {
    /* build a rule: match_subsystem=tty, match_product=10c4/glob */
    struct dev_rule r; memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "name", "esp32-serial") == 0);
    assert(dev_rule_set(&r, "match_subsystem", "tty") == 0);
    assert(dev_rule_set(&r, "match_product", "10c4/*") == 0);
    assert(dev_rule_set(&r, "on_add", "/usr/local/bin/up.sh") == 0);
    assert(dev_rule_set(&r, "on_remove", "/usr/local/bin/down.sh") == 0);
    assert(r.nmatch == 2);
    assert(strcmp(r.name, "esp32-serial") == 0);
    assert(strcmp(r.on_add, "/usr/local/bin/up.sh") == 0);
    /* match_subsystem stored uppercased as the uevent key SUBSYSTEM */
    assert(strcmp(r.mkey[0], "SUBSYSTEM") == 0);

    /* matching uevent */
    struct uevent ev; ev.n = 0;
    set(&ev, "ACTION", "add"); set(&ev, "SUBSYSTEM", "tty"); set(&ev, "PRODUCT", "10c4/ea60");
    assert(dev_rule_match(&r, &ev) == 1);

    /* wrong product -> no match */
    struct uevent ev2; ev2.n = 0;
    set(&ev2, "SUBSYSTEM", "tty"); set(&ev2, "PRODUCT", "1a86/7523");
    assert(dev_rule_match(&r, &ev2) == 0);

    /* absent key (no PRODUCT) -> no match */
    struct uevent ev3; ev3.n = 0;
    set(&ev3, "SUBSYSTEM", "tty");
    assert(dev_rule_match(&r, &ev3) == 0);

    /* glob ttyUSB* on DEVNAME */
    struct dev_rule r2; memset(&r2, 0, sizeof r2);
    dev_rule_set(&r2, "match_devname", "/dev/ttyUSB*");
    struct uevent ev4; ev4.n = 0; set(&ev4, "DEVNAME", "/dev/ttyUSB0");
    assert(dev_rule_match(&r2, &ev4) == 1);
    struct uevent ev5; ev5.n = 0; set(&ev5, "DEVNAME", "/dev/sda1");
    assert(dev_rule_match(&r2, &ev5) == 0);

    /* rule with zero match conditions matches nothing */
    struct dev_rule r3; memset(&r3, 0, sizeof r3);
    assert(dev_rule_match(&r3, &ev4) == 0);

    /* unknown key rejected */
    struct dev_rule r4; memset(&r4, 0, sizeof r4);
    assert(dev_rule_set(&r4, "bogus", "x") == -1);

    printf("test_dev_match: OK\n");
    return 0;
}
