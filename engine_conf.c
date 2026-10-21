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
};
