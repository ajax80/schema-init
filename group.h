#ifndef GROUP_H
#define GROUP_H

#include <stdint.h>

#define MAX_GROUPS   16
#define MAX_MEMBERS   8

typedef struct {
    char    name[64];
    char    member_name[MAX_MEMBERS][64];
    int     member_idx[MAX_MEMBERS];   /* resolved service indices, -1=none */
    int     member_count;
    uint8_t state;
} group_t;

/* parse .grp files from dir; returns number loaded */
int  groups_load(const char *dir, group_t *table, int max);

/* recompute each group's aggregate state from current service states */
void groups_update(group_t *gtable, int gcount,
                   const uint8_t *svc_states, int scount);

/* log one line about a group's state */
void group_log(const group_t *grp, const char *event);

#endif
