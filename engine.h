#ifndef ENGINE_H
#define ENGINE_H

#include <sys/types.h>

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

typedef struct {
    void (*loop_pressure)(void);
    void (*loop_tail)(void);
    int  (*ctl_extra)(int fd, char *line);
    void (*shut_log)(const char *msg);
    void (*on_sigcont)(void);
    int  (*on_reaped)(pid_t pid, int status);
} engine_hooks_t;

extern engine_hooks_t engine_hooks;

void engine_hook_noop(void);
int  engine_hook_ctl_none(int fd, char *line);
void engine_hook_log_none(const char *msg);
int  engine_hook_reaped_none(pid_t pid, int status);

#endif
