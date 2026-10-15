#include "group.h"
#include "schema.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

/* ── aggregate state priority ───────────────────────────────────────────
 *
 * A group's state is the worst-case view of its members:
 *   EXCISED  > FRICTION > RECOVERY > NEW_PROCESS/FULL_TRUST > stable
 *
 * Once all members are stable the group promotes to:
 *   FUNDAMENTAL  if all are FUNDAMENTAL or PERFECT
 *   SETTLED      if any are SETTLED
 *   PERFECT      if all are PERFECT
 */

static uint8_t aggregate(const uint8_t *states, const int *idxs, int count,
                         const uint8_t *svc_states, int scount) {
    int i, any_starting = 0, all_perfect = 1, any_settled = 0;

    if (count == 0) return STATE_PERFECT;

    for (i = 0; i < count; i++) {
        int idx = idxs[i];
        uint8_t s;
        if (idx < 0 || idx >= scount) return STATE_NEW_PROCESS;
        s = svc_states[idx];
        (void)states;

        if (s == STATE_EXCISED)                          return STATE_EXCISED;
        if (s == STATE_FRICTION)                         return STATE_FRICTION;
        if (s == STATE_RECOVERY)                         return STATE_RECOVERY;
        if (s == STATE_NEW_PROCESS || s == STATE_FULL_TRUST) any_starting = 1;
        if (s != STATE_PERFECT)                          all_perfect = 0;
        if (s == STATE_SETTLED)                          any_settled = 1;
    }

    if (any_starting)  return STATE_NEW_PROCESS;
    if (all_perfect)   return STATE_PERFECT;
    if (any_settled)   return STATE_SETTLED;
    return STATE_FUNDAMENTAL;
}

/* ── groups_update ──────────────────────────────────────────────────── */

void groups_update(group_t *gtable, int gcount,
                   const uint8_t *svc_states, int scount) {
    int i;
    uint8_t prev;
    for (i = 0; i < gcount; i++) {
        prev = gtable[i].state;
        gtable[i].state = aggregate(NULL,
                                    gtable[i].member_idx,
                                    gtable[i].member_count,
                                    svc_states, scount);
        if (gtable[i].state != prev)
            group_log(&gtable[i], "state-change");
    }
}

/* ── group_log ──────────────────────────────────────────────────────── */

void group_log(const group_t *grp, const char *event) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    const char *sname;

    switch (grp->state) {
        case STATE_FUNDAMENTAL: sname = "fundamental"; break;
        case STATE_SETTLED:     sname = "settled";     break;
        case STATE_PERFECT:     sname = "perfect";     break;
        case STATE_NEW_PROCESS: sname = "new-process"; break;
        case STATE_RECOVERY:    sname = "recovery";    break;
        case STATE_FRICTION:    sname = "friction";    break;
        case STATE_EXCISED:     sname = "excised";     break;
        default:                sname = "?";           break;
    }

    printf("[%02d:%02d:%02d] [group] %-20s  %-12s  state=%s  members=%d\n",
           t->tm_hour, t->tm_min, t->tm_sec,
           grp->name, event, sname, grp->member_count);
    fflush(stdout);
}

/* ── groups_load ────────────────────────────────────────────────────── */

int groups_load(const char *dir, group_t *table, int max) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    int count = 0;
    int warned = 0;

    if (!d) return 0;

    while ((ent = readdir(d))) {
        char path[512];
        FILE *f;
        char line[512];
        group_t *grp;
        size_t nlen = strlen(ent->d_name);

        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".grp") != 0)
            continue;

        if (count >= max) {
            if (!warned) {
                fprintf(stderr, "schema-init: WARNING — MAX_GROUPS (%d) reached; extra .grp files ignored\n", max);
                warned = 1;
            }
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        f = fopen(path, "r");
        if (!f) continue;

        grp = &table[count];
        memset(grp, 0, sizeof(*grp));
        for (int i = 0; i < MAX_MEMBERS; i++) grp->member_idx[i] = -1;
        grp->state = STATE_NEW_PROCESS;

        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            char *val;
            if (!eq) continue;
            *eq = 0;
            val = eq + 1;
            val[strcspn(val, "\r\n")] = 0;

            if (strcmp(line, "name") == 0)
                strncpy(grp->name, val, sizeof(grp->name) - 1);
            else if (strcmp(line, "member") == 0 && grp->member_count < MAX_MEMBERS)
                strncpy(grp->member_name[grp->member_count++], val, 63);
        }
        fclose(f);

        if (!grp->name[0]) continue;
        count++;
    }

    closedir(d);
    return count;
}
