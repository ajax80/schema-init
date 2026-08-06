#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char tmpl[] = "/tmp/schema-udev-testXXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);

    char p[256];
    snprintf(p, sizeof p, "%s/esp32.dev", dir);
    FILE *f = fopen(p, "w");
    fputs("# a comment line is skipped, even one containing = like this\n", f);
    fputs("name=esp32-serial\n", f);
    fputs("match_subsystem=tty\n", f);
    fputs("match_product=10c4/ea60\n", f);
    fputs("on_add=/bin/true\n", f);
    fputs("bogus_key=ignored-with-warning\n", f);
    fclose(f);

    struct dev_rule r;
    assert(dev_rule_load_file(p, &r) == 0);
    assert(strcmp(r.name, "esp32-serial") == 0);
    assert(r.nmatch == 2);
    assert(strcmp(r.on_add, "/bin/true") == 0);

    /* a non-.dev file in the dir is ignored by the directory loader */
    snprintf(p, sizeof p, "%s/README.txt", dir);
    f = fopen(p, "w"); fputs("name=notarule\n", f); fclose(f);

    struct dev_rule rules[MAX_RULES];
    int n = dev_rules_load_dir(dir, rules, MAX_RULES);
    assert(n == 1);
    assert(strcmp(rules[0].name, "esp32-serial") == 0);

    /* an all-comment .dev is inert: a commented line with '=' is still skipped,
       no rule keys are recognized, load_file returns 1, and the dir loader does
       not count it (the shipped example.dev is exactly this shape) */
    snprintf(p, sizeof p, "%s/inert.dev", dir);
    f = fopen(p, "w");
    fputs("# name=should-not-load\n", f);
    fputs("#match_subsystem=tty\n", f);
    fputs("   # indented comment\n", f);
    fputs("\n", f);
    fclose(f);
    struct dev_rule ir;
    assert(dev_rule_load_file(p, &ir) == 1);
    n = dev_rules_load_dir(dir, rules, MAX_RULES);
    assert(n == 1);
    assert(strcmp(rules[0].name, "esp32-serial") == 0);

    /* nonexistent file -> -1 */
    assert(dev_rule_load_file("/nonexistent/schema-udev/x.dev", &ir) == -1);

    /* missing dir -> 0 rules, no crash */
    assert(dev_rules_load_dir("/nonexistent/schema-udev/dir", rules, MAX_RULES) == 0);

    printf("test_dev_load: OK\n");
    return 0;
}
