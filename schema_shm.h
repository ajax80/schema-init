#ifndef SCHEMA_SHM_H
#define SCHEMA_SHM_H

#include <stdint.h>

#define SCHEMA_SHM_NAME       "/schema-init"
#define SCHEMA_SHM_MAX        64
#define SCHEMA_SHM_MAX_GROUPS 16

typedef struct {
    char    name[64];
    uint8_t state;
    uint8_t weight;
    int32_t child_pid;
    int32_t restart_count;
} shm_svc_t;

typedef struct {
    char    name[64];
    uint8_t state;
    int32_t member_count;
} shm_group_t;

typedef struct {
    uint32_t    seq;         /* bumped after each full write — reader detects updates */
    int32_t     count;
    shm_svc_t   svc[SCHEMA_SHM_MAX];
    int32_t     group_count;
    shm_group_t groups[SCHEMA_SHM_MAX_GROUPS];
} schema_shm_t;

#endif
