#include "../sdbus_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads a dissolved-policy file (argv[1]) and a golden TSV (argv[2]); evaluates
   every row's request through sdbus_policy.h and asserts the verdict matches the
   golden's column 0 (the felt_policy.py verdict). Any mismatch fails the build.
   Columns: verdict op uid gids interface member msgtype path destination
            dest_names name has_reply_serial  (empty == NULL/None). */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "read fail %s\n", path); exit(2); }
    buf[n] = '\0'; fclose(f);
    return buf;
}

/* split line into fields on '\t' in place; empty field -> NULL. returns count. */
static int split_tsv(char *line, char **fields, int max) {
    int n = 0;
    char *p = line;
    fields[n++] = (*p == '\t' || *p == '\0') ? NULL : p;
    while (*p && n < max) {
        if (*p == '\t') {
            *p = '\0';
            fields[n++] = (p[1] == '\t' || p[1] == '\0' || p[1] == '\n') ? NULL : p + 1;
        }
        p++;
    }
    return n;
}

/* split a csv field into up to max ints; returns count (0 if NULL/empty). */
static int split_ints(char *csv, int *out, int max) {
    if (!csv || !*csv) return 0;
    int n = 0;
    char *p = csv;
    while (*p && n < max) {
        out[n++] = atoi(p);
        char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    return n;
}

/* split a csv field into up to max string pointers (in place); returns count. */
static int split_strs(char *csv, const char **out, int max) {
    if (!csv || !*csv) return 0;
    int n = 0;
    char *p = csv;
    while (*p && n < max) {
        out[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s dissolved.txt golden.tsv\n", argv[0]); return 2; }
    char *pol_text = slurp(argv[1]);
    sdbus_policy *pol = sdbus_policy_parse(pol_text);
    if (!pol) { fprintf(stderr, "policy parse failed\n"); return 2; }

    FILE *g = fopen(argv[2], "r");
    if (!g) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }

    char *line = NULL; size_t cap = 0; ssize_t len;
    long total = 0, matched = 0, mismatched = 0;
    while ((len = getline(&line, &cap, g)) != -1) {
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        if (!*line) continue;
        char *f[12];
        int nf = split_tsv(line, f, 12);
        if (nf < 12) { fprintf(stderr, "short row (%d fields): %s\n", nf, line); return 2; }

        int gids[64];
        const char *dest_names[32];
        sdbus_req r = {0};
        r.op = f[1];
        r.uid = f[2] ? atoi(f[2]) : -1;
        r.n_gids = split_ints(f[3], gids, 64);
        r.gids = gids;
        r.interface = f[4];
        r.member = f[5];
        r.msgtype = f[6];
        r.path = f[7];
        r.destination = f[8];
        r.n_dest_names = split_strs(f[9], dest_names, 32);
        r.dest_names = dest_names;
        r.name = f[10];
        r.has_reply_serial = f[11] ? atoi(f[11]) : 0;

        const char *got = sdbus_policy_eval(pol, &r);
        const char *want = f[0];
        total++;
        if (strcmp(got, want) == 0) {
            matched++;
        } else {
            mismatched++;
            if (mismatched <= 10)
                fprintf(stderr, "MISMATCH: want=%s got=%s uid=%d dest=%s iface=%s member=%s type=%s\n",
                        want, got, r.uid, f[8] ? f[8] : "(nil)", f[4] ? f[4] : "(nil)",
                        f[5] ? f[5] : "(nil)", f[6] ? f[6] : "(nil)");
        }
    }
    free(line); fclose(g); sdbus_policy_free(pol); free(pol_text);

    printf("conformance: %ld/%ld verdicts matched\n", matched, total);
    assert(mismatched == 0);
    return 0;
}
