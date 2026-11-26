// compat-enforce probe: stands in for an imported unit's daemon and reports the
// execution context PID 1 actually gave it, one key=value per line, to
// /run/probe/<name>. Behavioural where a flag could lie: MDWE, LockPersonality,
// RestrictRealtime, RestrictSUIDSGID and RestrictNamespaces are each tried, not
// read. Every attempt is undone or happens in a throwaway child.
//
//   enforce-probe NAME [PATH...]     PATHs: stat'd and checked for a ro mount
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static FILE *out;

static void status_field(const char *key, const char *as)
{
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    size_t n = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, n) && line[n] == ':') {
            char *v = line + n + 1;
            v += strspn(v, " \t");
            fprintf(out, "%s=%s", as, v);   // keeps its newline
        }
    if (f)
        fclose(f);
}

static void ns(const char *which)
{
    char p[64], l[64] = "";
    snprintf(p, sizeof p, "/proc/self/ns/%s", which);
    ssize_t n = readlink(p, l, sizeof l - 1);
    fprintf(out, "ns_%s=%s\n", which, n > 0 ? (l[n] = 0, l) : "?");
}

static void ro(const char *key, const char *path)
{
    struct statvfs v;
    fprintf(out, "%s=%s\n", key, statvfs(path, &v) ? "missing" : (v.f_flag & ST_RDONLY) ? "1" : "0");
}

static void lim(const char *key, int r)
{
    struct rlimit l;
    getrlimit(r, &l);
    fprintf(out, "%s=%lld:%lld\n", key, (long long)l.rlim_cur, (long long)l.rlim_max);
}

// 1 when the attempt is refused, in a child so a success changes nothing here
static int refused_in_child(int what)
{
    pid_t p = fork();
    if (p == 0) {
        int ok = 0;
        if (what == 0)
            ok = unshare(CLONE_NEWUTS) == 0;
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

static void notify_ready(void)
{
    const char *s = getenv("NOTIFY_SOCKET");
    if (!s || !*s)
        return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, s, sizeof a.sun_path - 1);
    if (a.sun_path[0] == '@')
        a.sun_path[0] = 0;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sendto(fd, "READY=1", 7, 0, (struct sockaddr *)&a, sizeof a);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    char tmp[300], path[300];
    snprintf(tmp, sizeof tmp, "/run/probe/.%s.%d", argv[1], getpid());
    snprintf(path, sizeof path, "/run/probe/%s", argv[1]);
    out = fopen(tmp, "w");
    if (!out)
        return 3;

    fprintf(out, "uid=%d\ngid=%d\n", (int)getuid(), (int)getgid());
    gid_t g[64];
    int ng = getgroups(64, g);
    fprintf(out, "groups=");
    for (int i = 0; i < ng; i++)
        fprintf(out, "%s%d", i ? "," : "", (int)g[i]);
    fprintf(out, "\n");
    status_field("NoNewPrivs", "nnp");
    status_field("Seccomp", "seccomp");
    status_field("CapBnd", "capbnd");
    status_field("CapEff", "capeff");
    status_field("CapAmb", "capamb");
    mode_t m = umask(0);
    umask(m);
    fprintf(out, "umask=%04o\n", (unsigned)m);
    char cwd[256] = "?";
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "?");
    fprintf(out, "cwd=%s\n", cwd);
    lim("nofile", RLIMIT_NOFILE);
    lim("nproc", RLIMIT_NPROC);
    lim("core", RLIMIT_CORE);
    lim("memlock", RLIMIT_MEMLOCK);
    errno = 0;
    fprintf(out, "nice=%d\n", getpriority(PRIO_PROCESS, 0));
    FILE *f = fopen("/proc/self/oom_score_adj", "r");
    int oom = 0;
    if (f) {
        if (fscanf(f, "%d", &oom) != 1)
            oom = 0;
        fclose(f);
    }
    fprintf(out, "oom=%d\n", oom);
    // LimitNOFILE=infinity means this ceiling (systemd clamps to it)
    f = fopen("/proc/sys/fs/nr_open", "r");
    long nr = -1;
    if (f) {
        if (fscanf(f, "%ld", &nr) != 1)
            nr = -1;
        fclose(f);
    }
    fprintf(out, "nr_open=%ld\n", nr);
    ns("net");
    ns("uts");
    ns("mnt");
    ns("ipc");
    ns("cgroup");
    // markers the guest's init puts in place before any service starts
    fprintf(out, "tmp_marker=%d\n", access("/tmp/.enforce-marker", F_OK) == 0);
    fprintf(out, "home_marker=%d\n", access("/home/enforce/.marker", F_OK) == 0);
    fprintf(out, "root_marker=%d\n", access("/root/.marker", F_OK) == 0);
    fprintf(out, "dev_mem=%d\n", access("/dev/mem", F_OK) == 0);
    ro("ro_usr", "/usr");
    ro("ro_etc", "/etc");
    ro("ro_boot", "/boot");
    ro("ro_procsys", "/proc/sys");
    ro("ro_cgroup", "/sys/fs/cgroup");

    void *wx = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fprintf(out, "mdwe=%d\n", wx == MAP_FAILED);
    if (wx != MAP_FAILED)
        munmap(wx, 4096);
    int was = personality(0xffffffff);
    int pr = personality(PER_LINUX32);
    fprintf(out, "lockpers=%d\n", pr == -1);
    if (pr != -1)
        personality(was);
    struct sched_param sp = {.sched_priority = 1};
    int rt = sched_setscheduler(0, SCHED_FIFO, &sp);
    fprintf(out, "rt_refused=%d\n", rt != 0);
    if (rt == 0) {
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }
    char sf[64];
    snprintf(sf, sizeof sf, "/tmp/.suid.%d", getpid());
    int fd = open(sf, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    fprintf(out, "suid_refused=%s\n", fd < 0 ? "?" : fchmod(fd, 04755) ? "1" : "0");
    if (fd >= 0) {
        close(fd);
        unlink(sf);
    }
    fprintf(out, "unshare_refused=%d\n", refused_in_child(0));

    for (int i = 2; i < argc; i++) {
        struct stat st;
        struct statvfs v;
        if (stat(argv[i], &st))
            fprintf(out, "path=%s missing\n", argv[i]);
        else
            fprintf(out, "path=%s %d %d %04o %d\n", argv[i], (int)st.st_uid, (int)st.st_gid,
                    (unsigned)(st.st_mode & 07777), !statvfs(argv[i], &v) && (v.f_flag & ST_RDONLY));
    }
    for (char **e = environ; *e; e++)
        if (!strchr(*e, '\n'))
            fprintf(out, "env=%s\n", *e);
    fclose(out);
    rename(tmp, path);
    notify_ready();
    return 0;
}
