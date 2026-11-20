#include "svc_serial.h"
#include "sha256.h"
#include <stdarg.h>
#include <strings.h>

const char *svc_hard_source(const service_t *s, uint8_t bit) {
    if (s->hard_set & bit) return "explicit";
    if ((s->hard_dyn & bit) && !(s->hard_dropped & bit)) return "dynamic_user";
    if (s->hard_dropped & bit) return "dropped (default would hide exec/ready_path)";
    if (s->hard_default & bit) return "default";
    return "unset";
}

/* what syscall_deny= does, not how it was spelled: off/no/... is "0", on/yes/... the preset;
 * explicit or defaulted, so a default that starts applying it shows */
const char *svc_sc_deny_label(const service_t *s) {
    static const char *const off[] = { "0", "no", "off", "false", "none" }, *const on[] = { "default", "yes", "on", "true" };
    if (!((s->hard_set | s->hard_default) & HARD_SD)) return "-";
    /* a defaulted one never has its own spelling: it is the preset */
    if (!(s->hard_set & HARD_SD) && !s->sc_deny[0]) return "default";
    for (size_t i = 0; i < sizeof off / sizeof *off; i++) if (!strcasecmp(s->sc_deny, off[i])) return "0";
    for (size_t i = 0; i < sizeof on / sizeof *on; i++) if (!strcasecmp(s->sc_deny, on[i])) return "default";
    return "list";
}

const char *svc_protect_system_name(uint8_t v) {
    static const char *const ps[] = { "0", "1", "full" };
    return ps[v < 3 ? v : 0];
}

/* the baseline's three sources; a knob dynamic_user=1 implies wasn't written for that key */
static const char *src_of(const service_t *s, uint8_t bit) {
    if (s->hard_set & bit) return "explicit";
    if (s->hard_dropped & bit) return "dropped";
    if (s->hard_default & bit) return "default";
    return NULL;
}

typedef struct { char *p; size_t n, cap; int items; } sb_t;

static void sb_putc(sb_t *b, char c) {
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->p = realloc(b->p, b->cap);
    }
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

static void sb_esc(sb_t *b, const char *v, int item) {
    for (; *v; v++) {
        if (*v == '\\') { sb_putc(b, '\\'); sb_putc(b, '\\'); }
        else if (*v == '\n') { sb_putc(b, '\\'); sb_putc(b, 'n'); }
        else if (*v == '\t') { sb_putc(b, '\\'); sb_putc(b, 't'); }
        else if (*v == ' ' && item) { sb_putc(b, '\\'); sb_putc(b, 's'); }
        else sb_putc(b, *v);
    }
}

/* \e for an empty item: else ["", "a"] reads as ["a"] and [""] as [] */
static void sb_item(sb_t *b, const char *v) {
    if (b->items++) sb_putc(b, ' ');
    if (!*v) { sb_putc(b, '\\'); sb_putc(b, 'e'); }
    sb_esc(b, v, 1);
}

static void sb_itemf(sb_t *b, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    sb_item(b, t);
}

typedef struct { char **v; int n, cap; } lines_t;

static void add_raw(lines_t *L, const service_t *s, const char *key, const char *val, const char *src) {
    char *l;
    if (asprintf(&l, "%s.%s=%s%s%s", s->name, key, val ? val : "", src ? "\tsrc=" : "", src ? src : "") < 0) return;
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->v = realloc(L->v, (size_t)L->cap * sizeof *L->v);
    }
    L->v[L->n++] = l;
}

static void add_str(lines_t *L, const service_t *s, const char *key, const char *val) {
    sb_t b = { 0 };
    sb_esc(&b, val, 0);
    add_raw(L, s, key, b.p, NULL);
    free(b.p);
}

static void add_sb(lines_t *L, const service_t *s, const char *key, sb_t *b) {
    add_raw(L, s, key, b->p, NULL);
    free(b->p);
    *b = (sb_t){ 0 };
}

static void add_list(lines_t *L, const service_t *s, const char *key, char *const *v, int n) {
    sb_t b = { 0 };
    for (int i = 0; i < n; i++) if (v[i]) sb_item(&b, v[i]);
    add_sb(L, s, key, &b);
}

static void add_int(lines_t *L, const service_t *s, const char *key, long long v) {
    char t[32];
    snprintf(t, sizeof t, "%lld", v);
    add_raw(L, s, key, t, NULL);
}

static void add_mode(lines_t *L, const service_t *s, const char *key, int set, unsigned mode) {
    char t[16];
    snprintf(t, sizeof t, "%04o", mode);
    add_raw(L, s, key, set ? t : "-", NULL);
}

/* an out-of-range value is written as ?N, never as some valid name: a new
 * enum value must show as a change */
static void add_enum(lines_t *L, const service_t *s, const char *key, const char *const *names, int n, int v) {
    char t[16];
    snprintf(t, sizeof t, "?%d", v);
    add_raw(L, s, key, v >= 0 && v < n && names[v] ? names[v] : t, NULL);
}

static void em_exec(lines_t *L, const service_t *s) {
    int n = 0;
    while (n < MAX_ARGV && s->argv[n]) n++;
    add_str(L, s, "exec", s->exec);
    add_list(L, s, "argv", s->argv, n);
    add_int(L, s, "expand_args", s->expand_args);
    /* the +/- prefixes are part of each command as stored */
    add_list(L, s, "exec_pre", s->exec_pre, s->exec_pre_count);
    add_list(L, s, "exec_stop", s->exec_stop, s->exec_stop_count);
    add_list(L, s, "exec_stop_post", s->exec_stop_post, s->exec_stop_post_count);
    add_list(L, s, "exec_reload", s->exec_reload, s->exec_reload_count);
    add_int(L, s, "reload_signal", s->reload_signal);
    add_list(L, s, "card", s->card, s->card_count);
}

static void em_identity(lines_t *L, const service_t *s) {
    char t[32];
    sb_t b = { 0 };
    add_str(L, s, "user", s->run_user);
    add_str(L, s, "group", s->run_group);
    for (int i = 0; i < s->supp_count; i++) sb_item(&b, s->supp_groups[i]);
    add_sb(L, s, "supplementary_groups", &b);
    add_int(L, s, "dynamic_user", s->dynamic_user);
    snprintf(t, sizeof t, "0x%016llx", (unsigned long long)s->cap_keep_mask);
    add_raw(L, s, "keep_caps", s->cap_restrict ? t : "-", NULL);
    snprintf(t, sizeof t, "0x%016llx", (unsigned long long)s->cap_ambient_mask);
    add_raw(L, s, "ambient_caps", t, NULL);
}

static void em_env(lines_t *L, const service_t *s) {
    sb_t b = { 0 };
    add_list(L, s, "env", s->envp, s->env_count);
    for (int i = 0; i < s->env_file_count; i++) sb_item(&b, s->env_file[i]);
    add_sb(L, s, "env_file", &b);
}

static void em_fs(lines_t *L, const service_t *s) {
    char key[32];
    add_str(L, s, "working_dir", s->working_dir);
    for (int k = 0; k < EXEC_DIR_KINDS; k++) {
        add_list(L, s, service_exec_dir_key[k], s->exec_dir[k], s->exec_dir_count[k]);
        snprintf(key, sizeof key, "%s_mode", service_exec_dir_key[k]);
        add_mode(L, s, key, (s->exec_dir_mode_set >> k) & 1, s->exec_dir_mode[k]);
    }
    add_list(L, s, "rw_path", s->rw_path, s->rw_path_count);
    add_mode(L, s, "umask", s->umask_val >= 0, (unsigned)s->umask_val);
}

static void em_hard(lines_t *L, const service_t *s) {
    char t[8], ps[8];
    snprintf(ps, sizeof ps, "?%d", s->ns_protect_system);
    add_raw(L, s, "no_new_privs", (s->flags & SVC_NO_NEW_PRIVS) ? "1" : "0", src_of(s, HARD_NNP));
    snprintf(t, sizeof t, "%d", s->ns_private_tmp);
    add_raw(L, s, "private_tmp", t, src_of(s, HARD_PT));
    add_raw(L, s, "protect_system", s->ns_protect_system < 3 ? svc_protect_system_name(s->ns_protect_system) : ps,
            src_of(s, HARD_PS));
    snprintf(t, sizeof t, "%d", s->ns_protect_home);
    add_raw(L, s, "protect_home", t, src_of(s, HARD_PH));
    add_raw(L, s, "syscall_deny", svc_sc_deny_label(s), src_of(s, HARD_SD));
}

static void em_ns_kernel(lines_t *L, const service_t *s) {
    for (int k = 0; k < NS_K_COUNT; k++) add_int(L, s, ns_kernel_names[k], (s->ns_kernel >> k) & 1);
}

static void em_landlock(lines_t *L, const service_t *s) {
    static const char *const tk[2] = { "landlock_bind_tcp", "landlock_connect_tcp" };
    sb_t ro = { 0 }, rw = { 0 };
    for (int i = 0; i < s->landlock_count; i++) sb_item((s->landlock_rw >> i) & 1 ? &rw : &ro, s->landlock[i]);
    add_sb(L, s, "landlock_ro", &ro);
    add_sb(L, s, "landlock_rw", &rw);
    for (int k = 0; k < 2; k++) {
        sb_t b = { 0 };
        if (s->landlock_tcp_n[k] < 0) sb_item(&b, "-");
        else if (!s->landlock_tcp_n[k]) sb_item(&b, "none");
        for (int i = 0; i < s->landlock_tcp_n[k]; i++) sb_itemf(&b, "%u", s->landlock_tcp[k][i]);
        add_sb(L, s, tk[k], &b);
    }
}

/* sha256 of the set's syscall numbers on this arch, ascending, comma-joined */
static int sc_hash(const uint64_t *set, char out[65]) {
    int nr[SC_WORDS * 64], n = sc_nrs(set, nr);
    sb_t b = { 0 };
    char t[16];
    for (int i = 0; i < n; i++) {
        snprintf(t, sizeof t, i ? ",%d" : "%d", nr[i]);
        for (char *c = t; *c; c++) sb_putc(&b, *c);
    }
    sha256_hex(b.p ? b.p : "", b.n, out);
    free(b.p);
    return n;
}

static void em_sc(lines_t *L, const service_t *s) {
    static const char *const mode[] = { "none", "allow", "deny" };
    char h[65];
    sb_t b = { 0 };
    add_enum(L, s, "sc.mode", mode, 3, s->sc.mode);
    add_int(L, s, "sc.count", sc_hash(s->sc.set, h));
    add_raw(L, s, "sc.hash", h, NULL);
    for (int k = 0; k < s->sc.n_err && k < SC_MAX_ERRS; k++) {
        int n = sc_hash(s->sc.err_set[k], h);
        sb_itemf(&b, "%u:%d:%s", s->sc.err_val[k], n, h);
    }
    add_sb(L, s, "sc.errnos", &b);
    if (s->sc.err == SC_KILL) add_raw(L, s, "syscall_error_number", "kill", NULL);
    else add_int(L, s, "syscall_error_number", s->sc.err);
    add_raw(L, s, "syscall_architectures", s->sc.native ? "native" : "", NULL);
    add_int(L, s, "restrict_suid_sgid", s->sc.suid);
    add_int(L, s, "lock_personality", s->sc.persona);
    add_int(L, s, "restrict_realtime", s->sc.rt);
    add_int(L, s, "restrict_namespaces", s->sc.ns);
}

static void em_deps(lines_t *L, const service_t *s) {
    sb_t b = { 0 };
    for (int i = 0; i < MAX_DEPS && s->dep_name[i][0]; i++) sb_item(&b, s->dep_name[i]);
    add_sb(L, s, "dep", &b);
    add_str(L, s, "bind", s->bind_name);
}

static void em_flags(lines_t *L, const service_t *s) {
    static const struct { const char *k; int f; } fl[] = {
        { "oneshot", SVC_ONESHOT }, { "needs_root", SVC_NEEDS_ROOT }, { "critical", SVC_CRITICAL },
        { "stop_first", SVC_STOP_FIRST }, { "main_pid_notify", SVC_MAIN_PID_NOTIFY },
        { "persistent", SVC_TIMER_PERSIST },
    };
    for (size_t i = 0; i < sizeof fl / sizeof *fl; i++) add_int(L, s, fl[i].k, (s->flags & fl[i].f) != 0);
}

static void em_ready(lines_t *L, const service_t *s) {
    add_int(L, s, "notify", s->notify);
    add_str(L, s, "ready_bus_name", s->ready_bus_name);
    add_str(L, s, "ready_path", s->ready_path);
    add_str(L, s, "pid_file", s->pid_file);
    add_str(L, s, "wait_socket", s->wait_socket);
    add_int(L, s, "ready_poll_hz", s->ready_poll_hz);
    add_int(L, s, "stable_secs", s->stable_secs);
    add_int(L, s, "start_timeout_sec", s->start_timeout_sec);
    add_int(L, s, "fdstore", s->fdstore_max);
    add_str(L, s, "verify_memory_max", s->verify_memory_max);
    add_str(L, s, "verify_cpu_max", s->verify_cpu_max);
    add_str(L, s, "verify_pids_max", s->verify_pids_max);
}

static void status_list(sb_t *b, const uint8_t *codes, uint64_t sigs) {
    for (int c = 0; c < 256; c++) if (codes[c / 8] & (1u << (c % 8))) sb_itemf(b, "%d", c);
    for (int g = 1; g <= 64; g++) if (sigs & (1ULL << (g - 1))) sb_itemf(b, "SIG%d", g);
}

static void em_lifecycle(lines_t *L, const service_t *s) {
    static const char *const pol[] = { "always", "no", "on-success", "on-failure",
                                       "on-abnormal", "on-abort", "on-watchdog" };
    static const char *const km[] = { "default", "control-group", "mixed", "process" };
    sb_t b = { 0 };
    /* restart=no and no_restart=1 are both SVC_NO_RESTART */
    if (s->flags & SVC_NO_RESTART) add_raw(L, s, "restart", "no", NULL);
    else add_enum(L, s, "restart", pol, 7, s->restart_policy);
    status_list(&b, s->success_codes, s->success_sigs);
    add_sb(L, s, "success_exit_status", &b);
    status_list(&b, s->prevent_codes, s->prevent_sigs);
    add_sb(L, s, "restart_prevent_exit_status", &b);
    add_int(L, s, "restart_sec", s->restart_sec);
    add_int(L, s, "max_restarts", s->max_restarts);
    add_enum(L, s, "kill_mode", km, 4, s->kill_mode);
    add_int(L, s, "kill_signal", s->kill_signal);
    add_int(L, s, "final_kill_signal", s->final_kill_signal);
    add_int(L, s, "stop_timeout_sec", s->stop_timeout_sec);
    add_int(L, s, "watchdog_sec", s->watchdog_sec);
    add_int(L, s, "watchdog_timeout_ms", s->watchdog_timeout_ms);
    add_int(L, s, "no_excise", s->no_excise);
    add_int(L, s, "fuse", s->fuse);
    add_str(L, s, "fuse_cmd", s->fuse_cmd);
    add_str(L, s, "failsafe", s->failsafe_cmd);
    add_int(L, s, "failsafe_timeout_ms", s->failsafe_timeout_ms);
}

static void em_timers(lines_t *L, const service_t *s) {
    sb_t b = { 0 };
    add_int(L, s, "on_boot_sec", s->timer_boot_sec);
    add_int(L, s, "on_active_sec", s->timer_interval_sec);
    for (int i = 0; i < s->timer_cal_n && i < 4; i++)
        sb_itemf(&b, "dow=%02x,dom=%08x,h=%06x,m=%015llx", s->timer_cal[i].dow, s->timer_cal[i].dom,
                 s->timer_cal[i].hours, (unsigned long long)s->timer_cal[i].mins);
    add_sb(L, s, "on_calendar", &b);
    add_int(L, s, "randomized_delay_sec", s->timer_jitter_sec);
}

static void em_watch(lines_t *L, const service_t *s) {
    sb_t b = { 0 };
    for (int i = 0; i < s->watch_count && i < MAX_WATCH; i++)
        sb_itemf(&b, "%s:%s", watch_kind_name(s->watch[i].kind), s->watch[i].path);
    add_sb(L, s, "watch_path", &b);
    add_int(L, s, "make_directory", s->make_directory);
    add_mode(L, s, "directory_mode", 1, s->directory_mode);
    add_int(L, s, "trigger_limit_burst", s->trigger_burst);
    add_int(L, s, "trigger_limit_interval_sec", s->trigger_interval_sec);
}

static void em_sockets(lines_t *L, const service_t *s) {
    sb_t b = { 0 };
    for (int i = 0; i < s->listen_count && i < MAX_LISTEN; i++) sb_item(&b, s->listen[i]);
    add_sb(L, s, "listen", &b);
    add_int(L, s, "listen_lazy", s->listen_lazy);
    add_mode(L, s, "socket_mode", s->socket_mode != 0, (unsigned)s->socket_mode);
    add_str(L, s, "socket_user", s->socket_user);
    add_str(L, s, "socket_group", s->socket_group);
    for (int i = 0; i < s->cond_count && i < MAX_COND; i++) sb_item(&b, s->cond[i]);
    add_sb(L, s, "condition", &b);
}

static void em_resources(lines_t *L, const service_t *s) {
    static const char *const prio[] = { "peripheral", "standard", "critical" };
    static const char *const part[] = { "member", "root", "isolated" };
    static const char *const ioc[] = { "none", "realtime", "best-effort", "idle" };
    static const char *const pol[] = { "other", "fifo", "rr", "batch", NULL, "idle" };
    char t[16];
    add_enum(L, s, "priority", prio, 3, (int)s->priority);
    add_int(L, s, "cpu_limit", s->cpu_limit_pct);
    add_int(L, s, "mem_limit", s->mem_limit_mb);
    add_int(L, s, "mem_low", s->mem_low_mb);
    if (!s->swap_max_set) add_raw(L, s, "swap_max", "-", NULL);
    else if (s->swap_max_mb < 0) add_raw(L, s, "swap_max", "max", NULL);
    else add_int(L, s, "swap_max", s->swap_max_mb);
    add_int(L, s, "pids_max", s->pids_max);
    add_int(L, s, "cpu_weight", s->cpu_weight);
    add_str(L, s, "cpuset", s->cpuset);
    add_enum(L, s, "cpuset_partition", part, 3, s->cpuset_partition);
    add_int(L, s, "allowed_slot_min", s->allowed_slot_min);
    add_int(L, s, "allowed_slot_max", s->allowed_slot_max);
    snprintf(t, sizeof t, "%d", s->oom_score_adj);
    add_raw(L, s, "oom_score_adj", s->oom_adj_set ? t : "-", NULL);
    snprintf(t, sizeof t, "%d", s->nice_val);
    add_raw(L, s, "nice", s->nice_set ? t : "-", NULL);
    if (s->io_class < 0) add_raw(L, s, "io_class", "-", NULL);
    else add_enum(L, s, "io_class", ioc, 4, s->io_class);
    snprintf(t, sizeof t, "%d", s->io_prio);
    add_raw(L, s, "io_priority", s->io_prio >= 0 ? t : "-", NULL);
    if (s->sched_policy < 0) add_raw(L, s, "sched_policy", "-", NULL);
    else add_enum(L, s, "sched_policy", pol, 6, s->sched_policy);
    add_int(L, s, "sched_priority", s->sched_prio);
}

static void em_rlimits(lines_t *L, const service_t *s) {
    for (int i = 0; i < SERVICE_RLIM_KEYS; i++) {
        char t[64] = "-", c[24], m[24];
        for (int j = 0; j < s->rlim_count && j < 16; j++) {
            if (s->rlim[j].res != service_rlim_keys[i].res) continue;
            if (s->rlim[j].cur == RLIM_INFINITY) snprintf(c, sizeof c, "infinity");
            else snprintf(c, sizeof c, "%llu", (unsigned long long)s->rlim[j].cur);
            if (s->rlim[j].max == RLIM_INFINITY) snprintf(m, sizeof m, "infinity");
            else snprintf(m, sizeof m, "%llu", (unsigned long long)s->rlim[j].max);
            snprintf(t, sizeof t, "%s:%s", c, m);
        }
        add_raw(L, s, service_rlim_keys[i].key, t, NULL);
    }
}

/* Each row lists the .svc keys whose parsed effect its emitter writes;
 * tests/test_svc_serial_keys.sh fails on a parser key no row names. */
static const struct {
    void (*emit)(lines_t *, const service_t *);
    const char *keys[24];
} props[] = {
    { em_exec, { "exec", "args", "expand_args", "exec_pre", "exec_stop", "exec_stop_post", "exec_reload",
                 "reload", "reload_signal", "card" } },
    { em_identity, { "user", "group", "supplementary_groups", "dynamic_user", "keep_caps", "ambient_caps" } },
    { em_env, { "env", "env_file" } },
    { em_fs, { "working_dir", "runtime_dir", "state_dir", "cache_dir", "logs_dir", "runtime_dir_mode",
               "state_dir_mode", "cache_dir_mode", "logs_dir_mode", "rw_path", "umask" } },
    { em_hard, { "no_new_privs", "private_tmp", "protect_system", "protect_home", "syscall_deny" } },
    { em_ns_kernel, { "protect_kernel_tunables", "protect_kernel_modules", "protect_control_groups",
                      "private_devices", "protect_hostname", "private_network", "memory_deny_write_execute" } },
    { em_landlock, { "landlock_ro", "landlock_rw", "landlock_bind_tcp", "landlock_connect_tcp" } },
    { em_sc, { "syscall_filter", "syscall_error_number", "syscall_architectures", "restrict_suid_sgid",
               "lock_personality", "restrict_realtime", "restrict_namespaces" } },
    { em_deps, { "dep", "bind" } },
    { em_flags, { "oneshot", "needs_root", "critical", "stop_first", "main_pid_notify", "persistent" } },
    { em_ready, { "notify", "ready_bus_name", "ready_path", "pid_file", "wait_socket", "ready_poll_hz",
                  "stable_secs", "start_timeout_sec", "fdstore", "verify_memory_max", "verify_cpu_max",
                  "verify_pids_max" } },
    { em_lifecycle, { "restart", "no_restart", "success_exit_status", "restart_prevent_exit_status",
                      "restart_sec", "max_restarts", "kill_mode", "kill_signal", "final_kill_signal",
                      "stop_timeout_sec", "watchdog_sec", "watchdog_timeout_ms", "no_excise", "fuse",
                      "fuse_cmd", "failsafe", "failsafe_timeout_ms" } },
    { em_timers, { "on_boot_sec", "on_active_sec", "on_calendar", "randomized_delay_sec" } },
    { em_watch, { "watch_path", "make_directory", "directory_mode", "trigger_limit_burst",
                  "trigger_limit_interval_sec" } },
    { em_sockets, { "listen", "listen_lazy", "socket_mode", "socket_user", "socket_group", "condition" } },
    { em_resources, { "priority", "cpu_limit", "mem_limit", "mem_low", "swap_max", "pids_max", "cpu_weight",
                      "cpuset", "cpuset_partition", "allowed_slot_min", "allowed_slot_max", "oom_score_adj",
                      "nice", "io_class", "io_priority", "sched_policy", "sched_priority" } },
    { em_rlimits, { "limit_cpu", "limit_fsize", "limit_data", "limit_stack", "limit_core", "limit_rss",
                    "limit_nofile", "limit_as", "limit_nproc", "limit_memlock", "limit_locks",
                    "limit_sigpending", "limit_msgqueue", "limit_nice", "limit_rtprio", "limit_rttime" } },
};

static int line_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int flush(lines_t *L, FILE *out) {
    int n = L->n;
    /* qsort(NULL, 0) is UB: an empty table has no lines array */
    if (L->n) qsort(L->v, (size_t)L->n, sizeof *L->v, line_cmp);
    for (int i = 0; i < L->n; i++) {
        fprintf(out, "%s\n", L->v[i]);
        free(L->v[i]);
    }
    free(L->v);
    return n;
}

int svc_serialize_all(const service_t *tab, int n, FILE *out) {
    lines_t L = { 0 };
    for (int i = 0; i < n; i++)
        for (size_t p = 0; p < sizeof props / sizeof *props; p++) props[p].emit(&L, &tab[i]);
    return flush(&L, out);
}

int svc_serialize(const service_t *s, FILE *out) {
    return svc_serialize_all(s, 1, out);
}
