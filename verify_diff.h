#ifndef SCHEMA_VERIFY_DIFF_H
#define SCHEMA_VERIFY_DIFF_H

#define BASELINE_VERSION 1

typedef enum { F_FAIL, F_CHANGE, F_WARN, F_NOTE } fkind_t;
typedef struct { fkind_t kind; char text[512]; } finding_t;

enum { BASE_OK, BASE_NEVER, BASE_LOST };

/* Findings written up to MAX; returns how many there were in all, or -1 when
 * BASE is not a baseline this version can read (a lost baseline). */
int diff_baseline(const char *base, const char *next, finding_t *out, int max, char changes_hash[65]);
int baseline_state(const char *dir, char **base_out);
int accepted_matches(const char *dir, const char hash[65]);
int accept_write(const char *dir, const char hash[65]);

#endif
