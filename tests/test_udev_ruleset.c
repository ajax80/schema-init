#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    struct rule cr;
    const struct rule_clause *c;
#define CL(LINE) (ruleset_parse_line((LINE), &cr) == 1 ? (c = &cr.clause[0], 0) : -1)

    /* match, no subkey */
    assert(CL("ACTION==\"add\"") == 0);
    assert(strcmp(c->key, "ACTION") == 0 && c->subkey[0] == '\0' &&
           c->op == OP_MATCH_EQ && strcmp(c->val, "add") == 0);

    /* not-equal match */
    assert(CL("ACTION!=\"remove\"") == 0);
    assert(c->op == OP_MATCH_NE && strcmp(c->val, "remove") == 0);

    /* subkey with slash/glob */
    assert(CL("ATTR{parameters/events_dfl_poll_msecs}==\"0\"") == 0);
    assert(strcmp(c->key, "ATTR") == 0 &&
           strcmp(c->subkey, "parameters/events_dfl_poll_msecs") == 0 &&
           c->op == OP_MATCH_EQ && strcmp(c->val, "0") == 0);

    /* each assignment operator */
    assert(CL("OPTIONS+=\"watch\"") == 0 && c->op == OP_ASSIGN_ADD);
    assert(CL("TAG-=\"seat\"") == 0 && c->op == OP_ASSIGN_SUB);
    assert(CL("MODE=\"660\"") == 0 && c->op == OP_ASSIGN);
    assert(CL("ENV{ID_X}:=\"1\"") == 0 &&
           c->op == OP_ASSIGN_FINAL && strcmp(c->subkey, "ID_X") == 0);

    /* value containing | alternation and glob preserved verbatim */
    assert(CL("KERNEL==\"sd*|vd*\"") == 0 &&
           strcmp(c->val, "sd*|vd*") == 0);

    /* \" stays inside the value and is unescaped, as udev_rule_parse_value does */
    assert(CL("RUN+=\"bash -c '[[ $$x != \\\"D\\\" ]] && y'\"") == 0);
    assert(strcmp(c->val, "bash -c '[[ $$x != \"D\" ]] && y'") == 0);
    /* other backslashes are left for the value's own consumer */
    assert(CL("SYMLINK+=\"a\\x2db\"") == 0 && strcmp(c->val, "a\\x2db") == 0);

    /* malformed: no operator */
    assert(CL("ACTION add") == -1);

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

    /* a comma after \" is still inside the value, not a clause separator */
    struct rule q;
    assert(ruleset_parse_line("RUN+=\"a \\\"b,c\\\" d, e\", MODE=\"660\"", &q) == 2);
    assert(strcmp(q.clause[0].val, "a \"b,c\" d, e") == 0);
    assert(strcmp(q.clause[1].key, "MODE") == 0 && strcmp(q.clause[1].val, "660") == 0);
    free(q.clause);

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
    ruleset_free(&rs);
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
    ruleset_free(&rs2);
    unlink(pa); unlink(pb); unlink(pc); rmdir(d1); rmdir(d2);

    printf("test_udev_ruleset: dirs OK\n");

    /* reload keeps the live set when the new load fails or comes back empty:
     * a HUP while the rules dirs are unreadable must not leave the daemon
     * processing uevents with no rules */
    {
        char base[] = "/tmp/rs-reload-XXXXXX";
        assert(mkdtemp(base));
        char da[300], db[300], pr[340];
        snprintf(da, sizeof da, "%s/a", base);
        snprintf(db, sizeof db, "%s/b", base);
        assert(mkdir(da, 0755) == 0 && mkdir(db, 0755) == 0);
        snprintf(pr, sizeof pr, "%s/10-x.rules", da);
        FILE *f = fopen(pr, "w");
        fputs("KERNEL==\"sda\", SYMLINK+=\"x\"\n", f);
        fclose(f);

        const char *da_only[] = { da }, *db_only[] = { db };
        const char *gone[] = { "/nonexistent-schema-rs-a", "/nonexistent-schema-rs-b" };
        struct ruleset lv = {0};
        assert(ruleset_reload_swap(&lv, da_only, 1) == 0 && lv.n == 1);

        /* every dir unreadable -> refused, old rules stay */
        assert(ruleset_reload_swap(&lv, gone, 2) != 0 && lv.n == 1);
        assert(strcmp(lv.rules[0].clause[0].val, "sda") == 0);

        /* readable but empty while rules are live -> refused, old rules stay */
        assert(ruleset_reload_swap(&lv, db_only, 1) != 0 && lv.n == 1);

        /* a usable new set swaps in */
        snprintf(pr, sizeof pr, "%s/20-y.rules", db);
        f = fopen(pr, "w");
        fputs("KERNEL==\"vda\"\nKERNEL==\"vdb\"\n", f);
        fclose(f);
        assert(ruleset_reload_swap(&lv, db_only, 1) == 0 && lv.n == 2);
        assert(strcmp(lv.rules[0].clause[0].val, "vda") == 0);
        ruleset_free(&lv);

        /* first load into an empty set from empty dirs is not an error */
        struct ruleset fresh = {0};
        snprintf(pr, sizeof pr, "%s/20-y.rules", db);
        unlink(pr);
        assert(ruleset_reload_swap(&fresh, db_only, 1) == 0 && fresh.n == 0);
        ruleset_free(&fresh);

        snprintf(pr, sizeof pr, "%s/10-x.rules", da);
        unlink(pr);
        rmdir(da); rmdir(db); rmdir(base);
    }
    printf("test_udev_ruleset: reload-swap OK\n");

    /* smoke: real installed set loads and parses a sane number of rules */
    const char *real[] = { "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };
    struct ruleset live = {0};
    assert(ruleset_load_dirs(real, 3, &live) == 0);
    if (access("/usr/lib/udev/rules.d", F_OK) == 0)
        assert(live.n > 100);   /* 168 files on blakbox -> thousands of rules */
    ruleset_free(&live);

    printf("test_udev_ruleset: ALL OK\n");
    return 0;
}
