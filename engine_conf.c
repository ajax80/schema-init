#include "engine.h"
#include "notify.h"
#include "schema_shm.h"
#include "svc_dropins.h"

engine_paths_t engine_paths = {
    .ctl_sock        = CTL_SOCK_PATH,
    .run_dir         = "/run/schema-init",
    .notify_sock     = NOTIFY_SOCK_PATH,
    .shm_name        = SCHEMA_SHM_NAME,
    .timer_stamp_dir = TIMER_STAMP_DIR,
    .svc_dir         = SVC_DIR,
    .ctl_fallback    = "./run/schema-init.sock",
    .env_fallback    = "./run/schema-init/env",
    .svc_fallback    = "./services",
    .tag             = "schema-init",
};

void engine_hook_noop(void) {}
int  engine_hook_ctl_none(int fd, char *line) { (void)fd; (void)line; return 0; }
void engine_hook_log_none(const char *msg) { (void)msg; }
int  engine_hook_reaped_none(pid_t pid, int status) { (void)pid; (void)status; return 0; }

engine_hooks_t engine_hooks = {
    .loop_pressure = engine_hook_noop,
    .loop_tail     = engine_hook_noop,
    .ctl_extra     = engine_hook_ctl_none,
    .shut_log      = engine_hook_log_none,
    .on_sigcont    = engine_hook_noop,
    .on_reaped     = engine_hook_reaped_none,
    .after_sockets = engine_hook_noop,
};
