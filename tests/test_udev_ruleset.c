#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct rule_clause c;

    /* match, no subkey */
    assert(ruleset_parse_clause("ACTION==\"add\"", &c) == 0);
    assert(strcmp(c.key, "ACTION") == 0 && c.subkey[0] == '\0' &&
           c.op == OP_MATCH_EQ && strcmp(c.val, "add") == 0);

    /* not-equal match */
    assert(ruleset_parse_clause("ACTION!=\"remove\"", &c) == 0);
    assert(c.op == OP_MATCH_NE && strcmp(c.val, "remove") == 0);

    /* subkey with slash/glob */
    assert(ruleset_parse_clause("ATTR{parameters/events_dfl_poll_msecs}==\"0\"", &c) == 0);
    assert(strcmp(c.key, "ATTR") == 0 &&
           strcmp(c.subkey, "parameters/events_dfl_poll_msecs") == 0 &&
           c.op == OP_MATCH_EQ && strcmp(c.val, "0") == 0);

    /* each assignment operator */
    assert(ruleset_parse_clause("OPTIONS+=\"watch\"", &c) == 0 && c.op == OP_ASSIGN_ADD);
    assert(ruleset_parse_clause("TAG-=\"seat\"", &c) == 0 && c.op == OP_ASSIGN_SUB);
    assert(ruleset_parse_clause("MODE=\"660\"", &c) == 0 && c.op == OP_ASSIGN);
    assert(ruleset_parse_clause("ENV{ID_X}:=\"1\"", &c) == 0 &&
           c.op == OP_ASSIGN_FINAL && strcmp(c.subkey, "ID_X") == 0);

    /* value containing | alternation and glob preserved verbatim */
    assert(ruleset_parse_clause("KERNEL==\"sd*|vd*\"", &c) == 0 &&
           strcmp(c.val, "sd*|vd*") == 0);

    /* malformed: no operator */
    assert(ruleset_parse_clause("ACTION add", &c) == -1);

    printf("test_udev_ruleset: clause OK\n");
    return 0;
}
