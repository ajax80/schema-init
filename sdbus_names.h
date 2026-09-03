#ifndef SDBUS_NAMES_H
#define SDBUS_NAMES_H

/* Name registry: unique-name allocation, well-known ownership with a waiting
   queue, and RequestName/ReleaseName semantics per the D-Bus spec. Connections
   are opaque int conn_ids; the driver maps conn_id <-> ":1.N" for the wire.
   Name entries persist for the registry's life (bounded by distinct names) so
   transition.name pointers stay valid after a name goes unowned. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SDBUS_REQ_ALLOW_REPLACEMENT 0x1
#define SDBUS_REQ_REPLACE_EXISTING  0x2
#define SDBUS_REQ_DO_NOT_QUEUE      0x4

enum { SDBUS_REQ_PRIMARY_OWNER = 1, SDBUS_REQ_IN_QUEUE = 2,
       SDBUS_REQ_EXISTS = 3, SDBUS_REQ_ALREADY_OWNER = 4 };
enum { SDBUS_REL_RELEASED = 1, SDBUS_REL_NON_EXISTENT = 2, SDBUS_REL_NOT_OWNER = 3 };

typedef struct { const char *name; int old_owner, new_owner; } sdbus_transition;

typedef struct { int conn_id; unsigned flags; } sdbus_holder;
typedef struct {
    char *name;
    sdbus_holder *holders;   /* holders[0] == primary owner; rest == queue */
    int n_holders;
} sdbus_name_ent;
typedef struct { int conn_id; char *unique; } sdbus_unique_ent;

struct sdbus_names {
    sdbus_name_ent *names; int n_names;
    sdbus_unique_ent *uniques; int n_uniques;
    unsigned next_unique;
};
typedef struct sdbus_names sdbus_names;

static inline sdbus_names *sdbus_names_new(void) {
    sdbus_names *n = calloc(1, sizeof *n);
    if (n) n->next_unique = 1;
    return n;
}

static inline sdbus_name_ent *sdbus__name_find(sdbus_names *r, const char *name) {
    for (int i = 0; i < r->n_names; i++)
        if (!strcmp(r->names[i].name, name)) return &r->names[i];
    return NULL;
}

static inline sdbus_name_ent *sdbus__name_intern(sdbus_names *r, const char *name) {
    sdbus_name_ent *e = sdbus__name_find(r, name);
    if (e) return e;
    r->names = realloc(r->names, (r->n_names + 1) * sizeof *r->names);
    e = &r->names[r->n_names++];
    e->name = strdup(name);
    e->holders = NULL;
    e->n_holders = 0;
    return e;
}

static inline int sdbus__holder_index(sdbus_name_ent *e, int conn_id) {
    for (int i = 0; i < e->n_holders; i++)
        if (e->holders[i].conn_id == conn_id) return i;
    return -1;
}

static inline void sdbus__holder_insert(sdbus_name_ent *e, int idx, int conn_id, unsigned flags) {
    e->holders = realloc(e->holders, (e->n_holders + 1) * sizeof *e->holders);
    for (int i = e->n_holders; i > idx; i--) e->holders[i] = e->holders[i - 1];
    e->holders[idx].conn_id = conn_id;
    e->holders[idx].flags = flags;
    e->n_holders++;
}

static inline void sdbus__holder_remove(sdbus_name_ent *e, int idx) {
    for (int i = idx; i < e->n_holders - 1; i++) e->holders[i] = e->holders[i + 1];
    e->n_holders--;
}

static inline const char *sdbus_names_alloc_unique(sdbus_names *r, int conn_id) {
    char buf[32];
    snprintf(buf, sizeof buf, ":1.%u", r->next_unique++);
    r->uniques = realloc(r->uniques, (r->n_uniques + 1) * sizeof *r->uniques);
    r->uniques[r->n_uniques].conn_id = conn_id;
    r->uniques[r->n_uniques].unique = strdup(buf);
    return r->uniques[r->n_uniques++].unique;
}

/* conn_id of the current primary owner of `name`, or -1. */
static inline int sdbus_names_owner(sdbus_names *r, const char *name) {
    sdbus_name_ent *e = sdbus__name_find(r, name);
    if (!e || e->n_holders == 0) return -1;
    return e->holders[0].conn_id;
}

static inline int sdbus_names_request(sdbus_names *r, int conn_id, const char *well_known,
                               unsigned flags, sdbus_transition *out, int *n_out) {
    *n_out = 0;
    sdbus_name_ent *e = sdbus__name_intern(r, well_known);

    if (e->n_holders == 0) {                         /* free -> primary owner */
        sdbus__holder_insert(e, 0, conn_id, flags);
        out[0].name = e->name; out[0].old_owner = -1; out[0].new_owner = conn_id;
        *n_out = 1;
        return SDBUS_REQ_PRIMARY_OWNER;
    }

    int idx = sdbus__holder_index(e, conn_id);
    if (idx == 0) {                                  /* already primary owner */
        e->holders[0].flags = flags;
        return SDBUS_REQ_ALREADY_OWNER;
    }
    if (idx > 0) {                                   /* already waiting in queue */
        e->holders[idx].flags = flags;
        return SDBUS_REQ_IN_QUEUE;
    }

    /* someone else owns it */
    unsigned owner_flags = e->holders[0].flags;
    int old_owner = e->holders[0].conn_id;
    if ((flags & SDBUS_REQ_REPLACE_EXISTING) &&
        (owner_flags & SDBUS_REQ_ALLOW_REPLACEMENT)) {
        /* new request takes the name; old owner is queued unless it said
           DO_NOT_QUEUE */
        if (owner_flags & SDBUS_REQ_DO_NOT_QUEUE) {
            sdbus__holder_remove(e, 0);              /* drop old owner entirely */
            sdbus__holder_insert(e, 0, conn_id, flags);
        } else {
            sdbus__holder_insert(e, 0, conn_id, flags);   /* new primary at front */
            /* old owner now at index 1 already (shifted down) */
        }
        out[0].name = e->name; out[0].old_owner = old_owner; out[0].new_owner = conn_id;
        *n_out = 1;
        return SDBUS_REQ_PRIMARY_OWNER;
    }

    if (flags & SDBUS_REQ_DO_NOT_QUEUE)
        return SDBUS_REQ_EXISTS;
    sdbus__holder_insert(e, e->n_holders, conn_id, flags);   /* append to queue */
    return SDBUS_REQ_IN_QUEUE;
}

static inline int sdbus_names_release(sdbus_names *r, int conn_id, const char *well_known,
                               sdbus_transition *out, int *n_out) {
    *n_out = 0;
    sdbus_name_ent *e = sdbus__name_find(r, well_known);
    if (!e || e->n_holders == 0) return SDBUS_REL_NON_EXISTENT;
    int idx = sdbus__holder_index(e, conn_id);
    if (idx < 0) return SDBUS_REL_NOT_OWNER;
    if (idx == 0) {
        sdbus__holder_remove(e, 0);
        int newo = e->n_holders ? e->holders[0].conn_id : -1;
        out[0].name = e->name; out[0].old_owner = conn_id; out[0].new_owner = newo;
        *n_out = 1;
    } else {
        sdbus__holder_remove(e, idx);                /* silent queue removal */
    }
    return SDBUS_REL_RELEASED;
}

/* Drop conn from every name it holds; emit a transition for each name whose
   primary owner changes. out must have room for r->n_names entries. */
static inline void sdbus_names_disconnect(sdbus_names *r, int conn_id,
                                   sdbus_transition *out, int *n_out) {
    *n_out = 0;
    for (int i = 0; i < r->n_names; i++) {
        sdbus_name_ent *e = &r->names[i];
        int idx = sdbus__holder_index(e, conn_id);
        if (idx < 0) continue;
        if (idx == 0) {
            sdbus__holder_remove(e, 0);
            int newo = e->n_holders ? e->holders[0].conn_id : -1;
            out[*n_out].name = e->name;
            out[*n_out].old_owner = conn_id;
            out[*n_out].new_owner = newo;
            (*n_out)++;
        } else {
            sdbus__holder_remove(e, idx);
        }
    }
    /* forget this conn's unique name */
    for (int i = 0; i < r->n_uniques; i++) {
        if (r->uniques[i].conn_id == conn_id) {
            free(r->uniques[i].unique);
            r->uniques[i] = r->uniques[--r->n_uniques];
            break;
        }
    }
}

/* All currently-owned well-known names. Caller frees the returned array (not the
   strings). Returns count. */
static inline int sdbus_names_list(sdbus_names *r, const char ***out_names) {
    const char **arr = NULL;
    int n = 0;
    for (int i = 0; i < r->n_names; i++) {
        if (r->names[i].n_holders == 0) continue;
        arr = realloc(arr, (n + 1) * sizeof *arr);
        arr[n++] = r->names[i].name;
    }
    *out_names = arr;
    return n;
}

/* unique name string for a conn, or NULL. */
static inline const char *sdbus_names_unique(sdbus_names *r, int conn_id) {
    for (int i = 0; i < r->n_uniques; i++)
        if (r->uniques[i].conn_id == conn_id) return r->uniques[i].unique;
    return NULL;
}

static inline void sdbus_names_free(sdbus_names *r) {
    if (!r) return;
    for (int i = 0; i < r->n_names; i++) { free(r->names[i].name); free(r->names[i].holders); }
    for (int i = 0; i < r->n_uniques; i++) free(r->uniques[i].unique);
    free(r->names); free(r->uniques); free(r);
}

#endif
