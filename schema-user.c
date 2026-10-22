/* schema-user: the shared engine as an unprivileged per-user supervisor. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "engine.h"
#include "schema.h"
#include "service.h"

#define TAG "[schema-user] "

static char run_dir[PATH_MAX], ctl_sock[PATH_MAX], notify_sock[PATH_MAX], svc_dir[PATH_MAX];
static char shm_name[64], stamp_dir[PATH_MAX], log_dir[PATH_MAX];
static char cg_root[PATH_MAX];

static void die(const char *msg) {
    fprintf(stderr, "schema-user: %s\n", msg);
    exit(1);
}

static void join(char *dst, const char *a, const char *b) {
    if (snprintf(dst, PATH_MAX, "%s%s", a, b) >= PATH_MAX)
        die("path too long");
}

static void runtime_dir_check(const char *xdg) {
    struct stat st;
    char msg[PATH_MAX + 64];
    if (!xdg || !xdg[0])
        die("XDG_RUNTIME_DIR is not set");
    if (stat(xdg, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        snprintf(msg, sizeof msg, "XDG_RUNTIME_DIR %s is not a directory owned by uid %u",
                 xdg, (unsigned)geteuid());
        die(msg);
    }
}

/* The lock settles two starters racing (later, logind and a hand run); the
 * connect only finds whom to name. The lock fd stays open for our lifetime and
 * is not inherited by services. */
static void single_instance(void) {
    char lock[PATH_MAX], msg[128];
    struct sockaddr_un a;
    struct ucred cred;
    socklen_t cl = sizeof cred;
    int fd;
    join(lock, run_dir, "/lock");
    fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) die("cannot open the instance lock");
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, ctl_sock, strlen(ctl_sock) + 1);   /* length checked in main */
    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0)
        snprintf(msg, sizeof msg, "already running (pid %d)", (int)cred.pid);
    else
        snprintf(msg, sizeof msg, "already running");
    die(msg);
}

/* $VAR / ${VAR} against the current environment; no other shell syntax. */
static void envd_expand(const char *in, char *out, size_t n) {
    size_t o = 0;
    while (*in && o + 1 < n) {
        if (*in != '$') { out[o++] = *in++; continue; }
        char name[256];
        size_t k = 0;
        int brace = in[1] == '{';
        const char *p = in + 1 + brace;
        while ((*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (k && *p >= '0' && *p <= '9')) && k + 1 < sizeof name)
            name[k++] = *p++;
        name[k] = '\0';
        if (!k || (brace && *p != '}')) { out[o++] = *in++; continue; }
        in = p + brace;
        const char *v = getenv(name);
        while (v && *v && o + 1 < n) out[o++] = *v++;
    }
    out[o] = '\0';
}

static int conf_filter(const struct dirent *e) {
    size_t l = strlen(e->d_name);
    return l > 5 && !strcmp(e->d_name + l - 5, ".conf");
}

static void envd_replay_dir(const char *dir) {
    struct dirent **ents;
    int n = scandir(dir, &ents, conf_filter, alphasort);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX], line[4096], val[4096];
        FILE *f;
        snprintf(path, sizeof path, "%s/%s", dir, ents[i]->d_name);
        free(ents[i]);
        if (!(f = fopen(path, "re"))) continue;
        while (fgets(line, sizeof line, f)) {
            char *k = line + strspn(line, " \t"), *eq;
            k[strcspn(k, "\r\n")] = '\0';
            if (!*k || *k == '#' || !(eq = strchr(k, '=')) || eq == k) continue;
            *eq = '\0';
            envd_expand(eq + 1, val, sizeof val);
            setenv(k, val, 1);
        }
        fclose(f);
    }
    if (n >= 0) free(ents);
}

/* Services see the spec's base set plus environment.d, never the environment
 * of whoever launched us (a desktop terminal would leak WAYLAND_DISPLAY). */
static void service_environment(const char *xdg, const char *cfg_home) {
    struct passwd *pw = getpwuid(geteuid());
    char path[PATH_MAX], bus[PATH_MAX + 16], cfg[PATH_MAX];
    const char *p = getenv("PATH");
    if (!pw) die("no passwd entry for our uid");
    snprintf(path, sizeof path, "%s", p && p[0] ? p : "/usr/local/bin:/usr/bin:/bin");
    snprintf(bus, sizeof bus, "unix:path=%s/bus", xdg);
    snprintf(cfg, sizeof cfg, "%s", cfg_home);
    char xdg_copy[PATH_MAX];
    snprintf(xdg_copy, sizeof xdg_copy, "%s", xdg);
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("PATH", path, 1);
    setenv("XDG_RUNTIME_DIR", xdg_copy, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
    envd_replay_dir("/usr/lib/environment.d");
    envd_replay_dir("/etc/environment.d");
    join(path, cfg, "/environment.d");
    envd_replay_dir(path);
}

static int write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t w = write(fd, s, strlen(s));
    close(fd);
    return w == (ssize_t)strlen(s) ? 0 : -1;
}

/* Use the cgroup we were started in only if it was delegated to us: we can
 * write its cgroup.procs and create children in it. We move ourselves into
 * init.scope first, because cgroup v2 refuses to enable controllers for
 * children of a cgroup that still holds processes. */
static void user_cgroup_root(void) {
    char line[PATH_MAX], path[PATH_MAX], ctrl[256];
    const char *why = "not delegated to us";
    FILE *f = fopen("/proc/self/cgroup", "re");
    service_cgroup_root = "";
    cg_root[0] = '\0';
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "0::", 3)) {
            line[strcspn(line, "\n")] = '\0';
            if (snprintf(cg_root, sizeof cg_root, "/sys/fs/cgroup%s", line + 3) >= (int)sizeof cg_root)
                cg_root[0] = '\0';
        }
    if (f) fclose(f);
    if (!cg_root[0])
        why = "no cgroup v2";
    else if (strlen(cg_root) > sizeof(((service_t *)0)->cgroup_path) - 1 - sizeof(((service_t *)0)->name))
        why = "cgroup path too long";
    else {
        join(path, cg_root, "/cgroup.procs");
        if (access(path, W_OK) == 0 && access(cg_root, W_OK) == 0) {
            char pid[24];
            join(path, cg_root, "/init.scope");
            mkdir(path, 0755);
            join(path, cg_root, "/init.scope/cgroup.procs");
            snprintf(pid, sizeof pid, "%d\n", (int)getpid());
            if (write_str(path, pid) == 0) {
                join(path, cg_root, "/cgroup.controllers");
                if ((f = fopen(path, "re"))) {
                    char sub[PATH_MAX];
                    join(sub, cg_root, "/cgroup.subtree_control");
                    while (fscanf(f, "%250s", ctrl + 1) == 1) {
                        ctrl[0] = '+';
                        write_str(sub, ctrl);
                    }
                    fclose(f);
                }
                service_cgroup_root = cg_root;
                return;
            }
            why = "cannot move into init.scope";
        }
    }
    printf(TAG "no delegated cgroup (%s): running without per-service cgroups\n", why);
}

/* No subreaper process may be left once we exit. Without a cgroup an orphan
 * a service double-forked away belongs to no service's stop, so whatever was
 * reparented to us is killed here, repeatedly, as each kill can reparent its
 * own children. */
static void kill_orphans(void) {
    for (int round = 0; round < 10; round++) {
        int found = 0;
        DIR *d = opendir("/proc");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char path[300], buf[512], *rp;
            int fd, ppid;
            ssize_t n;
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
            if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) continue;
            n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            /* comm may hold spaces and ')': ppid follows the last ')' */
            if (!(rp = strrchr(buf, ')')) || sscanf(rp + 1, " %*c %d", &ppid) != 1) continue;
            if (ppid == getpid()) {
                kill(atoi(e->d_name), SIGKILL);
                found = 1;
            }
        }
        if (d) closedir(d);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (!found) return;
        usleep(20000);
    }
}

/* poll() would sleep through a flag set between the loop test and the call:
 * the SIGCONT lands on the engine's signalfd and wakes it. */
static void sig_stop(int s) {
    (void)s;
    engine_running = 0;
    kill(getpid(), SIGCONT);
}

#define IMPORT_DEFAULT "/usr/libexec/schema-init/schema-import"
#define IMPORT_SECS 10
#define IMPORT_LOG_MAX (256 * 1024)

/* Generate the services dir from the user's unit files. Login must never
 * depend on it: a failing or hung importer is logged and we run with
 * whatever the dir holds. */
static void run_import(const char *imp, const char *cfg_home, const char *state_home) {
    char logp[PATH_MAX];
    if (!imp[0]) {
        printf(TAG "import: not run (SCHEMA_USER_IMPORT is empty)\n");
        return;
    }
    if (access(imp, X_OK) != 0) {
        printf(TAG "import: not run (%s: %s)\n", imp, strerror(errno));
        return;
    }
    join(logp, log_dir, "/import.log");
    struct stat st;
    if (stat(logp, &st) == 0 && st.st_size > IMPORT_LOG_MAX) {
        char old[PATH_MAX];
        join(old, log_dir, "/import.log.old");
        rename(logp, old);
    }
    pid_t pid = fork();
    if (pid < 0) {
        printf(TAG "import: not run (fork: %s)\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int out = open(logp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        setenv("XDG_CONFIG_HOME", cfg_home, 1);
        setenv("XDG_STATE_HOME", state_home, 1);
        execl(imp, imp, "--user", "--out", svc_dir, (char *)NULL);
        _exit(127);
    }
    int status;
    /* a logout during the import must not wait out IMPORT_SECS: the importer
     * swaps its output in atomically, so killing it leaves the old set */
    for (int i = 0; i < IMPORT_SECS * 20 && engine_running; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (WIFEXITED(status))
                printf(TAG "import: exit %d\n", WEXITSTATUS(status));
            else
                printf(TAG "import: killed by signal %d\n", WTERMSIG(status));
            return;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    if (engine_running)
        printf(TAG "import: killed after %d s\n", IMPORT_SECS);
    else
        printf(TAG "import: killed, stopping\n");
}

static void user_shut_log(const char *msg) {
    printf(TAG "%s\n", msg);
}

int main(int argc, char **argv) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *state = getenv("XDG_STATE_HOME"), *cfg = getenv("XDG_CONFIG_HOME");
    const char *imp_env = getenv("SCHEMA_USER_IMPORT");
    char imp[PATH_MAX];
    char state_home[PATH_MAX], cfg_home[PATH_MAX];
    struct passwd *pw;
    struct sigaction sa;

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: schema-user\n"
               "Per-user supervisor. Runs the services in $XDG_RUNTIME_DIR/schema-user/services;\n"
               "drive it with schema-ctl --user.\n");
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
        printf("schema-user %s\n", SCHEMA_INIT_VERSION);
        return 0;
    }
    if (argc > 1) die("takes no arguments (see -h)");
    if (geteuid() == 0) die("schema-user runs as a user; PID 1 supervises root");
    /* read before service_environment() clears the environment */
    snprintf(imp, sizeof imp, "%s", imp_env ? imp_env : IMPORT_DEFAULT);
    runtime_dir_check(xdg);

    pw = getpwuid(geteuid());
    if (!pw) die("no passwd entry for our uid");
    if (state && state[0]) snprintf(state_home, sizeof state_home, "%s", state);
    else join(state_home, pw->pw_dir, "/.local/state");
    if (cfg && cfg[0]) snprintf(cfg_home, sizeof cfg_home, "%s", cfg);
    else join(cfg_home, pw->pw_dir, "/.config");

    join(run_dir, xdg, "/schema-user");
    join(ctl_sock, run_dir, "/ctl.sock");
    join(notify_sock, run_dir, "/notify");
    join(svc_dir, run_dir, "/services");
    if (strlen(ctl_sock) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        die("XDG_RUNTIME_DIR is too long for a socket path");
    join(stamp_dir, state_home, "/schema-user/timers");
    join(log_dir, state_home, "/schema-user/log");
    snprintf(shm_name, sizeof shm_name, "/schema-user-%u", (unsigned)geteuid());
    if (engine_mkdir_p(run_dir, 0700) < 0 || engine_mkdir_p(svc_dir, 0700) < 0)
        die("cannot create the run directory");
    single_instance();

    prctl(PR_SET_CHILD_SUBREAPER, 1);
    umask(022);
    setvbuf(stdout, NULL, _IOLBF, 0);

    engine_paths.run_dir         = run_dir;
    engine_paths.ctl_sock        = ctl_sock;
    engine_paths.notify_sock     = notify_sock;
    engine_paths.svc_dir         = svc_dir;
    engine_paths.shm_name        = shm_name;
    engine_paths.timer_stamp_dir = stamp_dir;
    engine_paths.ctl_fallback    = NULL;
    engine_paths.env_fallback    = NULL;
    engine_paths.svc_fallback    = NULL;
    engine_paths.tag             = "schema-user";
    engine_reload_integrity      = 0;
    if (engine_mkdir_p(log_dir, 0700) == 0)
        service_log_dir = log_dir;
    else
        die("cannot create the log directory");

    service_environment(xdg, cfg_home);
    user_cgroup_root();
    service_set_hardening_default(0);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    engine_hooks.shut_log = user_shut_log;

    run_import(imp, cfg_home, state_home);
    if (!engine_running)
        _exit(0);
    if (engine_load(svc_dir) > 0)
        die("dependency cycle in the service configuration");
    engine_open(0);
    engine_arm_timers(0);
    engine_run();
    engine_stop_services();
    engine_kill_services();
    kill_orphans();
    engine_release();
    _exit(0);
}
