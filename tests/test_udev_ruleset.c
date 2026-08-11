#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

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

    struct rule r;
    int nc = ruleset_parse_line(
        "ACTION==\"change\", SUBSYSTEM==\"block\", KERNEL==\"loop*\", GROUP=\"disk\", MODE=\"660\"", &r);
    assert(nc == 5 && r.nclause == 5);
    assert(strcmp(r.clause[0].key, "ACTION") == 0 && r.clause[0].op == OP_MATCH_EQ);
    assert(strcmp(r.clause[3].key, "GROUP") == 0 && strcmp(r.clause[3].val, "disk") == 0);
    assert(strcmp(r.clause[4].key, "MODE") == 0 && strcmp(r.clause[4].val, "660") == 0);

    /* GOTO / LABEL are single-clause lines */
    assert(ruleset_parse_line("ACTION==\"remove\", GOTO=\"uaccess_end\"", &r) == 2);
    assert(strcmp(r.clause[1].key, "GOTO") == 0 && strcmp(r.clause[1].val, "uaccess_end") == 0);
    assert(ruleset_parse_line("LABEL=\"uaccess_end\"", &r) == 1);

    /* blank line -> 0 clauses */
    assert(ruleset_parse_line("   ", &r) == 0);

    printf("test_udev_ruleset: line OK\n");

    /* file with a comment, a continuation, and two rules */
    char tmpl[] = "/tmp/schema-ruleset-XXXXXX";
    int fd = mkstemp(tmpl); assert(fd >= 0);
    const char *content =
        "# a comment\n"
        "ACTION!=\"remove\", SUBSYSTEM==\"block\", \\\n"
        "  KERNEL==\"sd*|vd*\", OPTIONS+=\"watch\"\n"
        "\n"
        "LABEL=\"end\"\n";
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);

    struct ruleset rs = {0};
    assert(ruleset_load_file(tmpl, &rs) == 0);
    assert(rs.n == 2);
    assert(rs.rules[0].nclause == 4);
    assert(strcmp(rs.rules[0].clause[2].key, "KERNEL") == 0 &&
           strcmp(rs.rules[0].clause[2].val, "sd*|vd*") == 0);
    assert(strcmp(rs.rules[1].clause[0].key, "LABEL") == 0);
    free(rs.rules);
    unlink(tmpl);

    printf("test_udev_ruleset: file OK\n");

    /* two dirs; /etc overrides /usr/lib for same basename; lexical order across names */
    char d1[] = "/tmp/schema-rd1-XXXXXX"; assert(mkdtemp(d1));
    char d2[] = "/tmp/schema-rd2-XXXXXX"; assert(mkdtemp(d2));
    char pa[256], pb[256], pc[256];
    snprintf(pa, sizeof pa, "%s/50-a.rules", d1);
    snprintf(pb, sizeof pb, "%s/90-z.rules", d1);
    snprintf(pc, sizeof pc, "%s/50-a.rules", d2);   /* overrides d1's 50-a */
    FILE *fa = fopen(pa, "w"); fputs("KERNEL==\"fromd1\"\n", fa); fclose(fa);
    FILE *fb = fopen(pb, "w"); fputs("KERNEL==\"zlast\"\n", fb); fclose(fb);
    FILE *fc = fopen(pc, "w"); fputs("KERNEL==\"fromd2\"\n", fc); fclose(fc);

    const char *dirs[] = { d1, d2 };   /* d2 = higher precedence */
    struct ruleset rs2 = {0};
    assert(ruleset_load_dirs(dirs, 2, &rs2) == 0);
    assert(rs2.n == 2);
    /* 50-a comes before 90-z lexically; 50-a resolved from d2 */
    assert(strcmp(rs2.rules[0].clause[0].val, "fromd2") == 0);
    assert(strcmp(rs2.rules[1].clause[0].val, "zlast") == 0);
    free(rs2.rules);
    unlink(pa); unlink(pb); unlink(pc); rmdir(d1); rmdir(d2);

    printf("test_udev_ruleset: dirs OK\n");
    return 0;
}
