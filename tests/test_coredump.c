#include "../coredump.h"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static char dir[] = "/tmp/schema-coredump-XXXXXX";

/* Run the helper as the kernel would, core bytes on stdin. */
static int run_helper(pid_t pid, unsigned long long climit, long t, size_t bytes) {
    int p[2];
    assert(pipe(p) == 0);
    pid_t h = fork();
    if (h == 0) {
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        char a[7][32];
        snprintf(a[0], 32, "%d", (int)pid);
        snprintf(a[1], 32, "%d", (int)getuid());
        snprintf(a[2], 32, "%d", (int)getgid());
        snprintf(a[3], 32, "%d", SIGSEGV);
        snprintf(a[4], 32, "%ld", t);
        snprintf(a[5], 32, "%llu", climit);
        snprintf(a[6], 32, "1");
        setenv("SCHEMA_COREDUMP_DIR", dir, 1);
        execl("./schema-coredump", "schema-coredump", a[0], a[1], a[2], a[3], a[4], a[5], a[6], (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    static char buf[65536];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 7);
    for (size_t off = 0; off < bytes; off += sizeof buf) {
        size_t n = bytes - off < sizeof buf ? bytes - off : sizeof buf;
        if (write(p[1], buf, n) != (ssize_t)n) break;
    }
    close(p[1]);
    int st;
    waitpid(h, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int count(const char *suffix) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name), sl = strlen(suffix);
        if (l > sl && !strcmp(de->d_name + l - sl, suffix)) n++;
    }
    closedir(d);
    return n;
}

static int newest_meta_has(const char *needle) {
    DIR *d = opendir(dir);
    struct dirent *de;
    time_t best = 0;
    char path[512] = "";
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 5, ".meta")) continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        stat(p, &st);
        if (st.st_mtime >= best) { best = st.st_mtime; snprintf(path, sizeof path, "%s", p); }
    }
    closedir(d);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char text[4096];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);             /* the helper stops reading at its cap */
    /* ---- pattern takeover: systemd-coredump's or the kernel default, never someone else's ---- */
    assert(coredump_pattern_classify("|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h %d %F\n") == 1);
    assert(coredump_pattern_classify("|/usr/lib/systemd/systemd-coredump") == 1);
    assert(coredump_pattern_classify(COREDUMP_PATTERN "\n") == 2);
    assert(coredump_pattern_classify("core\n") == 1);
    assert(coredump_pattern_classify("core") == 1);
    assert(coredump_pattern_classify("core.%p\n") == 0);
    assert(coredump_pattern_classify("corex\n") == 0);
    assert(coredump_pattern_classify("|/usr/share/apport/apport -p%p -s%s -c%c\n") == 0);
    assert(coredump_pattern_classify("|/usr/libexec/abrt-hook-ccpp %s %c %p %u %g %t e %P %I %h\n") == 0);
    assert(coredump_pattern_classify("|/usr/lib/systemd/systemd-coredump-evil %P\n") == 0);
    assert(coredump_pattern_classify("/var/crash/core.%e.%p\n") == 0);
    assert(strlen(COREDUMP_PATTERN) < 128);          /* kernel CORENAME_MAX_SIZE */
    printf("test_coredump pattern: OK\n");

    /* ---- names and values ---- */
    char svc[64];
    coredump_service_from_cgroup("0::/schema-init/sshd\n", svc, sizeof svc);
    assert(!strcmp(svc, "sshd"));
    coredump_service_from_cgroup("0::/schema-init/nm/worker\n", svc, sizeof svc);
    assert(!strcmp(svc, "nm"));
    coredump_service_from_cgroup("0::/user.slice/x\n", svc, sizeof svc);
    assert(!svc[0]);
    coredump_service_from_cgroup("1:x:/y\n0::/schema-init/crond\n", svc, sizeof svc);
    assert(!strcmp(svc, "crond"));
    char nm[64] = "../x y/\x01z";
    coredump_clean_name(nm, sizeof nm);
    assert(!strcmp(nm, "_._x_y__z") && !strchr(nm, '/'));
    char empty[16] = "";
    coredump_clean_name(empty, sizeof empty);
    assert(!strcmp(empty, "unknown"));
    char v[32] = "a\nb\x1b[2Jc";
    coredump_clean_value(v);
    assert(!strchr(v, '\n') && !strchr(v, '\x1b'));
    char sc[64];
    coredump_stem_comm("core.a.b.1000.1790743203.42.zst", sc, sizeof sc);
    assert(!strcmp(sc, "a.b"));
    coredump_stem_comm("core.a.1000.1790743203.42.meta", sc, sizeof sc);
    assert(!strcmp(sc, "a"));
    coredump_stem_comm("core.1.2.3", sc, sizeof sc);
    assert(!sc[0]);
    printf("test_coredump names: OK\n");

    /* ---- the helper end to end, against a live stand-in process ---- */
    assert(mkdtemp(dir));
    pid_t victim = fork();
    if (victim == 0) { pause(); _exit(0); }
    long now = (long)time(NULL);

    assert(run_helper(victim, ~0ULL, now, 3 << 20) == 0);
    assert(count(".zst") == 1 && count(".meta") == 1);
    assert(newest_meta_has("REASON=stored\n") && newest_meta_has("SIGNAL_NAME=SIGSEGV\n"));
    assert(newest_meta_has("CORE_RAW_BYTES=3145728\n") && newest_meta_has("TRUNCATED=0\n"));
    struct stat st;
    assert(stat(dir, &st) == 0 && (st.st_mode & 077) == 0);
    printf("test_coredump store: OK\n");

    /* a crash loop: the second core from the same program is not kept */
    sleep(1);
    assert(run_helper(victim, ~0ULL, now + 1, 1 << 20) == 0);
    assert(count(".zst") == 1 && count(".meta") == 2 && newest_meta_has("REASON=rate-limited\n"));
    printf("test_coredump rate limit: OK\n");

    /* RLIMIT_CORE 0: metadata only */
    sleep(1);
    assert(run_helper(victim, 0, now + 2, 1 << 20) == 0);
    assert(count(".zst") == 1 && count(".meta") == 3 && newest_meta_has("REASON=rlimit\n"));
    printf("test_coredump rlimit: OK\n");

    /* a nonzero limit caps the stored core */
    sleep(1);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -f %s/*.zst", dir);
    assert(system(cmd) == 0);
    assert(run_helper(victim, 1 << 20, now + 3, 3 << 20) == 0);
    assert(count(".zst") == 1 && newest_meta_has("CORE_RAW_BYTES=1048576\n") && newest_meta_has("TRUNCATED=1\n"));
    printf("test_coredump cap: OK\n");

    /* a helper killed mid-write leaves a temp file; the next run removes it */
    char stale[600];
    snprintf(stale, sizeof stale, "%s/.tmp.99999", dir);
    FILE *sf = fopen(stale, "w");
    fputs("partial", sf);
    fclose(sf);
    sleep(1);
    assert(run_helper(victim, 0, now + 5, 4096) == 0);
    assert(access(stale, F_OK) != 0);
    printf("test_coredump stale tmp: OK\n");

    /* a directory someone else can write is refused */
    chmod(dir, 0777);
    assert(run_helper(victim, ~0ULL, now + 4, 4096) == 1);
    chmod(dir, 0700);
    printf("test_coredump dir perms: OK\n");

    kill(victim, SIGKILL);
    waitpid(victim, NULL, 0);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
    printf("test_coredump: ALL OK\n");
    return 0;
}
