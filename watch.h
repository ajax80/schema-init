#ifndef SCHEMA_WATCH_H
#define SCHEMA_WATCH_H

#include <stdint.h>
#include <sys/inotify.h>
#include <sys/types.h>

#define MAX_WATCH 8

enum { WATCH_EXISTS, WATCH_EXISTS_GLOB, WATCH_DIR_NOT_EMPTY, WATCH_CHANGED, WATCH_MODIFIED };

typedef struct {
    uint8_t kind;
    uint8_t leaf_seen;   /* leaf existed at the last arm: absent→present fires edge kinds */
    int     wd_parent;   /* deepest existing ancestor dir (the glob's dir itself for exists_glob), -1 none */
    int     wd_leaf;     /* PATH itself, -1 when absent or kind is exists/exists_glob */
    int     depth;       /* components from the wd_parent dir down to the target: 1 = direct parent */
    char    path[256];
} watch_t;

#define WATCH_EV_NONE  0
#define WATCH_EV_FIRE  1
#define WATCH_EV_REARM 2

int  watch_parse(const char *val, watch_t *w);
const char *watch_kind_name(uint8_t kind);
int  watch_is_level(const watch_t *w);
void watch_arm(int ino_fd, watch_t *w);
int  watch_rearm(int ino_fd, watch_t *w);
int  watch_level_true(const watch_t *w);
int  watch_event(const watch_t *w, const struct inotify_event *ev);
int  watch_make_dir(const watch_t *w, mode_t mode);
void watch_gc(int ino_fd, watch_t *const *live, int n);

#endif
