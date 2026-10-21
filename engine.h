#ifndef ENGINE_H
#define ENGINE_H

#define CTL_SOCK_PATH   "/run/schema-init.sock"
#define TIMER_STAMP_DIR "/var/lib/schema-init/timers"

typedef struct {
    const char *ctl_sock;
    const char *run_dir;
    const char *notify_sock;
    const char *shm_name;
    const char *timer_stamp_dir;
    const char *svc_dir;
} engine_paths_t;

extern engine_paths_t engine_paths;

#endif
