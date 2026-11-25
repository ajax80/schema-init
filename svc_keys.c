#include "svc_keys.h"

/* Every key svc_serial.c writes after "<name>.", so schema-ctl can split a
 * line (a name may hold ".") without linking the service stack.
 * tests/test_svc_serial.c fails when this and the emitters disagree. */
const char *const svc_keys[] = {
    "after", "allowed_slot_max", "allowed_slot_min", "ambient_caps", "argv", "before", "bind",
    "cache_dir", "cache_dir_mode", "card", "condition", "cpu_limit",
    "cpu_weight", "cpuset", "cpuset_partition", "critical", "dep",
    "directory_mode", "dynamic_user", "env", "env_file", "exec",
    "exec_pre", "exec_reload", "exec_stop", "exec_stop_post", "expand_args",
    "failsafe", "failsafe_timeout_ms", "fdstore", "final_kill_signal", "fuse",
    "fuse_cmd", "group", "io_class", "io_priority", "keep_caps",
    "kill_mode", "kill_signal", "landlock_bind_tcp", "landlock_connect_tcp", "landlock_ro",
    "landlock_rw", "limit_as", "limit_core", "limit_cpu", "limit_data",
    "limit_fsize", "limit_locks", "limit_memlock", "limit_msgqueue", "limit_nice",
    "limit_nofile", "limit_nproc", "limit_rss", "limit_rtprio", "limit_rttime",
    "limit_sigpending", "limit_stack", "listen", "listen_lazy", "lock_personality",
    "logs_dir", "logs_dir_mode", "main_pid_notify", "make_directory", "max_restarts",
    "mem_limit", "mem_low", "memory_deny_write_execute", "needs_root", "nice",
    "no_excise", "no_new_privs", "notify", "on_active_sec", "on_boot_sec",
    "on_calendar", "oneshot", "oom_score_adj", "persistent", "pid_file",
    "pids_max", "priority", "private_devices", "private_network", "private_tmp",
    "protect_control_groups", "protect_home", "protect_hostname", "protect_kernel_modules", "protect_kernel_tunables",
    "protect_system", "randomized_delay_sec", "ready_bus_name", "ready_path", "ready_poll_hz",
    "reload_signal", "restart", "restart_prevent_exit_status", "restart_sec", "restrict_namespaces",
    "restrict_realtime", "restrict_suid_sgid", "runtime_dir", "runtime_dir_mode", "rw_path",
    "sc.count", "sc.errnos", "sc.hash", "sc.mode", "sched_policy",
    "sched_priority", "socket_group", "socket_mode", "socket_user", "stable_secs",
    "start_timeout_sec", "state_dir", "state_dir_mode", "stop_first", "stop_timeout_sec",
    "success_exit_status", "supplementary_groups", "swap_max", "syscall_architectures", "syscall_deny",
    "syscall_error_number", "trigger_limit_burst", "trigger_limit_interval_sec", "umask", "user",
    "verify_cpu_max", "verify_memory_max", "verify_pids_max", "wait_socket", "watch_path",
    "watchdog_sec", "watchdog_timeout_ms", "working_dir",
};
const int svc_keys_n = sizeof svc_keys / sizeof *svc_keys;
