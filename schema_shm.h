#ifndef SCHEMA_SHM_H
#define SCHEMA_SHM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SCHEMA_SHM_NAME       "/schema-init"
#define SCHEMA_SHM_MAX        128
#define SCHEMA_SHM_MAX_GROUPS 16
#define SCHEMA_SHM_MAGIC      0x53434d03u   /* "SCM", layout 3: 128 slots, cards */

typedef struct {
    char    name[64];
    uint8_t state;
    uint8_t weight;
    int32_t child_pid;
    int32_t restart_count;
    uint8_t card_played;
    uint8_t card_count;
    uint8_t limp;           /* SHM_LIMP_* */
} shm_svc_t;

#define SHM_LIMP_STANDBY   1   /* named by a card=fallback:, held until played */
#define SHM_LIMP_STANDIN   2   /* that standby, started by the card */
#define SHM_LIMP_FALLBACK  4   /* this service's fallback card is in play */

typedef struct {
    char    name[64];
    uint8_t state;
    int32_t member_count;
} shm_group_t;

typedef struct {
    uint32_t    magic;
    uint32_t    seq;         /* bumped after each full write — reader detects updates */
    int32_t     count;
    shm_svc_t   svc[SCHEMA_SHM_MAX];
    int32_t     group_count;
    shm_group_t groups[SCHEMA_SHM_MAX_GROUPS];
    uint8_t     system_state; /* 0=running, 13=shutdown, 14=restart */
} schema_shm_t;

/* Map PID 1's table read-only from an open fd; NULL if it is not this
 * layout (a different schema-init build, or not schema-init). */
static inline const schema_shm_t *schema_shm_map(int fd) {
    struct stat st;
    const schema_shm_t *m;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof *m) return NULL;
    m = mmap(NULL, sizeof *m, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return NULL;
    if (m->magic != SCHEMA_SHM_MAGIC) { munmap((void *)m, sizeof *m); return NULL; }
    return m;
}

#endif
