#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void write_svc(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/schema-svc-envXXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    char p[256];
    service_t svc;

    /* multiple env= lines, one KEY=VALUE each */
    snprintf(p, sizeof p, "%s/a.svc", dir);
    write_svc(p,
        "name=a\n"
        "exec=/bin/true\n"
        "env=FOO=bar\n"
        "env=PATH=/usr/bin:/bin\n"
        "args=-x\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_count == 2);
    assert(strcmp(svc.envp[0], "FOO=bar") == 0);
    assert(strcmp(svc.envp[1], "PATH=/usr/bin:/bin") == 0);

    /* a value containing '=' is kept whole; a malformed (no '=') env is dropped */
    snprintf(p, sizeof p, "%s/b.svc", dir);
    write_svc(p,
        "name=b\n"
        "exec=/bin/true\n"
        "env=KEY=a=b=c\n"
        "env=NOEQUALS\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_count == 1);
    assert(strcmp(svc.envp[0], "KEY=a=b=c") == 0);

    /* no env= lines -> empty */
    snprintf(p, sizeof p, "%s/c.svc", dir);
    write_svc(p, "name=c\nexec=/bin/true\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_count == 0);

    /* leading whitespace before the key is stripped */
    snprintf(p, sizeof p, "%s/d.svc", dir);
    write_svc(p, "name=d\nexec=/bin/true\nenv= \tFOO=bar\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_count == 1);
    assert(strcmp(svc.envp[0], "FOO=bar") == 0);

    /* all MAX_ENV entries are retained (no reserved trailing slot) */
    snprintf(p, sizeof p, "%s/e.svc", dir);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs("name=e\nexec=/bin/true\n", f);
    for (int i = 0; i < MAX_ENV; i++)
        fprintf(f, "env=K%d=v\n", i);
    fclose(f);
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_count == MAX_ENV);

    /* exec= with inline args is refused by both parsers */
    snprintf(p, sizeof p, "%s/f.svc", dir);
    write_svc(p, "name=f\nexec=/usr/bin/foo --bar\n");
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/g.svc", dir);
    write_svc(p, "name=g\nexec=/usr/bin/foo\targs\n");
    assert(service_load_one(p, &svc) == -1);

    char tmpl2[] = "/tmp/schema-svc-execXXXXXX";
    char *dir2 = mkdtemp(tmpl2);
    assert(dir2);
    snprintf(p, sizeof p, "%s/ok.svc", dir2);
    write_svc(p, "name=ok\nexec=/bin/true\nargs=-x\n");
    snprintf(p, sizeof p, "%s/bad.svc", dir2);
    write_svc(p, "name=bad\nexec=/bin/true -x\n");
    static service_t table[4];
    assert(services_load(dir2, table, 4) == 1);
    assert(strcmp(table[0].name, "ok") == 0);

    char tmpl3[] = "/tmp/schema-svc-efXXXXXX";
    char *dir3 = mkdtemp(tmpl3);
    assert(dir3);
    snprintf(p, sizeof p, "%s/h.svc", dir3);
    write_svc(p,
        "name=h\nexec=/bin/true\n"
        "env_file=/etc/sysconfig/h\n"
        "env_file=-/etc/default/h\n"
        "expand_args=1\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.env_file_count == 2);
    assert(strcmp(svc.env_file[0], "/etc/sysconfig/h") == 0);
    assert(strcmp(svc.env_file[1], "-/etc/default/h") == 0);
    assert(svc.expand_args == 1);
    snprintf(p, sizeof p, "%s/i.svc", dir3);
    write_svc(p, "name=i\nexec=/bin/true\nenv_file=relative/h\n");
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/k.svc", dir3);
    write_svc(p, "name=k\nexec=/bin/true\nenv_file=/a\nenv_file=/b\nenv_file=/c\nenv_file=/d\nenv_file=/e\n");
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/l.svc", dir3);
    {
        char longp[400] = "name=l\nexec=/bin/true\nenv_file=/";
        size_t l = strlen(longp);
        memset(longp + l, 'x', 270);
        strcpy(longp + l + 270, "\n");
        write_svc(p, longp);
    }
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/j.svc", dir3);
    write_svc(p, "name=j\nexec=/bin/true\nenv_file=-\n");
    assert(service_load_one(p, &svc) == -1);

    snprintf(p, sizeof p, "%s/env", dir3);
    write_svc(p,
        "# comment\n"
        "; also comment\n"
        "\n"
        "OPTIONS=\"-u chrony -F 2\"\n"
        "  SPACED = plain value   \n"
        "SINGLE='a \"b\" $c'\n"
        "DQ=\"x\\\"y\\\\z \\$w\"\n"
        "export EXP=1\n"
        "CONT=one\\\n"
        "two\n"
        "EMPTY=\n"
        "MIX=a'b c'\"d\"\n"
        "1BAD=no\n"
        "NOEQ\n"
        "LAST=end");
    char *pairs[32];
    int n = service_env_file_read(p, pairs, 32);
    const char *want[] = {
        "OPTIONS=-u chrony -F 2", "SPACED=plain value", "SINGLE=a \"b\" $c",
        "DQ=x\"y\\z $w", "EXP=1", "CONT=onetwo", "EMPTY=", "MIX=ab cd", "LAST=end",
    };
    assert(n == (int)(sizeof want / sizeof want[0]));
    for (int i = 0; i < n; i++) assert(strcmp(pairs[i], want[i]) == 0);
    assert(service_env_file_read(p, pairs, 2) == 2);
    assert(service_env_file_read("/nonexistent/schema-env", pairs, 32) == -1);

    setenv("OPTS", " -a  -b\tc ", 1);
    setenv("ONE", "x y", 1);
    unsetenv("UNSET_V");
    char *in[] = {"/bin/d", "$OPTS", "${ONE}", "--k=${ONE}!", "$UNSET_V", "${UNSET_V}",
                  "pre$ONE", "$$HOME", "${bad-name}", "${ONE", NULL};
    char *out[32];
    int m = service_expand_argv(in, out, 31);
    const char *exp[] = {"/bin/d", "-a", "-b", "c", "x y", "--k=x y!", "",
                         "pre$ONE", "$HOME", "${bad-name}", "${ONE"};
    assert(m == (int)(sizeof exp / sizeof exp[0]));
    for (int i = 0; i < m; i++) assert(strcmp(out[i], exp[i]) == 0);
    assert(service_expand_argv(in, out, 3) == 3);

    char *w[32];
    int nw = service_split_cmdline("  /usr/bin/bash -c \"pkill abrt-dbus || :\"  'a b'c\\ d \"x\\\"y\" ", w, 32);
    const char *wexp[] = {"/usr/bin/bash", "-c", "pkill abrt-dbus || :", "a bc d", "x\"y"};
    assert(nw == 5);
    for (int i = 0; i < nw; i++) assert(strcmp(w[i], wexp[i]) == 0);
    assert(service_split_cmdline("/bin/sh -c \"unterminated", w, 32) == -1);
    assert(service_split_cmdline("/bin/sh 'x", w, 32) == -1);
    assert(service_split_cmdline("   ", w, 32) == 0);
    assert(service_split_cmdline("a b c d", w, 2) == 2);

    snprintf(p, sizeof p, "%s/m.svc", dir3);
    write_svc(p,
        "name=m\nexec=/bin/true\n"
        "exec_pre=+-/bin/chown -f -R root:sssd /etc/sssd\n"
        "exec_pre=/bin/sh -c \"echo hi > /run/x\"\n"
        "exec_pre= -/usr/sbin/modprobe vboxguest\n");
    assert(service_load_one(p, &svc) == 0);
    assert(svc.exec_pre_count == 3);
    assert(strcmp(svc.exec_pre[0], "+-/bin/chown -f -R root:sssd /etc/sssd") == 0);
    assert(strcmp(svc.exec_pre[2], "-/usr/sbin/modprobe vboxguest") == 0);
    snprintf(p, sizeof p, "%s/n.svc", dir3);
    write_svc(p, "name=n\nexec=/bin/true\nexec_pre=-udevadm settle\n");
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/o.svc", dir3);
    write_svc(p, "name=o\nexec=/bin/true\nexec_pre=/bin/sh -c \"oops\n");
    assert(service_load_one(p, &svc) == -1);
    snprintf(p, sizeof p, "%s/q.svc", dir3);
    f = fopen(p, "w");
    assert(f);
    fputs("name=q\nexec=/bin/true\n", f);
    for (int i = 0; i <= MAX_EXEC_PRE; i++)
        fputs("exec_pre=/bin/true\n", f);
    fclose(f);
    assert(service_load_one(p, &svc) == -1);

    {
        struct sockaddr_storage ss;
        socklen_t len;
        int type;
        assert(service_listen_parse("stream:/run/cups/cups.sock", &type, &ss, &len) == 0);
        assert(type == SOCK_STREAM && ss.ss_family == AF_UNIX && len == sizeof(struct sockaddr_un));
        assert(service_listen_parse("seqpacket:@ISCSIADM_ABSTRACT_NAMESPACE", &type, &ss, &len) == 0);
        assert(type == SOCK_SEQPACKET && ((struct sockaddr_un *)&ss)->sun_path[0] == '\0');
        assert(len == offsetof(struct sockaddr_un, sun_path) + strlen("@ISCSIADM_ABSTRACT_NAMESPACE"));
        assert(service_listen_parse("fifo:/run/dmeventd-server", &type, &ss, &len) == 0 && type == 0);
        assert(service_listen_parse("dgram:631", &type, &ss, &len) == 0 && ss.ss_family == AF_INET6);
        assert(ntohs(((struct sockaddr_in6 *)&ss)->sin6_port) == 631);
        assert(service_listen_parse("stream:127.0.0.1:8080", &type, &ss, &len) == 0 && ss.ss_family == AF_INET);
        assert(service_listen_parse("stream:[::1]:8080", &type, &ss, &len) == 0 && ss.ss_family == AF_INET6);
        const char *bad[] = { "/run/x", "tcp:/run/x", "stream:", "stream:run/x", "fifo:@x", "fifo:80",
                              "stream:@", "stream:0", "stream:65536", "stream:1.2.3.4:", "stream:[::1]80",
                              "stream:nothost:80", "seqpacket:80", NULL };
        for (int i = 0; bad[i]; i++) assert(service_listen_parse(bad[i], &type, &ss, &len) == -1);

        snprintf(p, sizeof p, "%s/s.svc", dir3);
        write_svc(p, "name=s\nexec=/bin/true\nlisten=stream:/run/s.sock\nlisten=fifo:/run/s.fifo\n"
                     "listen_lazy=1\nsocket_mode=0660\nsocket_user=root\nsocket_group=wheel\n");
        assert(service_load_one(p, &svc) == 0);
        assert(svc.listen_count == 2 && svc.listen_lazy && svc.listen_hold && svc.listen_open == 0);
        assert(svc.socket_mode == 0660 && !strcmp(svc.socket_group, "wheel"));
        snprintf(p, sizeof p, "%s/t.svc", dir3);
        write_svc(p, "name=t\nexec=/bin/true\nlisten=stream:/run/a\nlisten=stream:/run/b\n"
                     "listen=stream:/run/c\nlisten=stream:/run/d\nlisten=stream:/run/e\n");
        assert(service_load_one(p, &svc) == -1);
        write_svc(p, "name=t\nexec=/bin/true\nlisten=unix:/run/a\n");
        assert(service_load_one(p, &svc) == -1);
        write_svc(p, "name=t\nexec=/bin/true\nsocket_mode=rw\n");
        assert(service_load_one(p, &svc) == -1);

        /* PID 1 side for real: bind, spawn, the child gets the socket at fd 3 */
        char sock[200], fifo[200], body[1024];
        snprintf(sock, sizeof sock, "%s/sub/l.sock", dir3);
        snprintf(fifo, sizeof fifo, "%s/sub/l.fifo", dir3);
        snprintf(p, sizeof p, "%s/l.svc", dir3);
        snprintf(body, sizeof body,
            "name=l\nexec=/usr/bin/python3\nargs=-c\n"
            "args=import os,socket,stat;s=socket.socket(fileno=3);c,_=s.accept();c.sendall(('%%s %%s %%s %%d' %% "
            "(os.environ['LISTEN_FDS'],os.environ['LISTEN_PID']==str(os.getpid()),os.environ['LISTEN_FDNAMES'],"
            "stat.S_ISFIFO(os.fstat(4).st_mode))).encode())\n"
            "listen=stream:%s\nlisten=fifo:%s\nsocket_mode=0600\n", sock, fifo);
        write_svc(p, body);
        assert(service_load_one(p, &svc) == 0);
        int first = -1;
        for (int round = 0; round < 2; round++) {
            assert(service_spawn(&svc) == 0);
            assert(svc.listen_open == 2);
            if (round == 0) first = svc.listen_fd[0];
            assert(svc.listen_fd[0] == first);
            struct stat st;
            assert(stat(sock, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 07777) == 0600);
            assert(stat(fifo, &st) == 0 && S_ISFIFO(st.st_mode) && (st.st_mode & 07777) == 0600);
            int c = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un un = { .sun_family = AF_UNIX };
            snprintf(un.sun_path, sizeof un.sun_path, "%s", sock);
            assert(connect(c, (struct sockaddr *)&un, sizeof un) == 0);
            char got[128] = "";
            ssize_t r, n = 0;
            while ((r = read(c, got + n, sizeof got - 1 - n)) > 0) n += r;
            close(c);
            int st2;
            assert(waitpid(svc.child_pid, &st2, 0) == svc.child_pid && WIFEXITED(st2) && WEXITSTATUS(st2) == 0);
            assert(strcmp(got, "2 True l.socket:l.socket 1") == 0);
        }
        assert(fcntl(first, F_GETFD) & FD_CLOEXEC);
        assert(service_listen_matches(&svc, 0) && service_listen_matches(&svc, 1));
        char keep[112];
        memcpy(keep, svc.listen[0], sizeof keep);
        snprintf(svc.listen[0], sizeof svc.listen[0], "stream:%s/sub/other.sock", dir3);
        assert(!service_listen_matches(&svc, 0));
        snprintf(svc.listen[0], sizeof svc.listen[0], "dgram:%s", sock);
        assert(!service_listen_matches(&svc, 0));
        memcpy(svc.listen[0], keep, sizeof keep);
        snprintf(svc.listen[1], sizeof svc.listen[1], "fifo:%s/sub/other.fifo", dir3);
        assert(!service_listen_matches(&svc, 1));
        service_listen_close(&svc);
        assert(svc.listen_open == 0 && fcntl(first, F_GETFD) == -1);

        /* a directory someone else can write to is refused, and nothing is
         * followed through a planted symlink */
        char open_dir[200], target[200];
        snprintf(open_dir, sizeof open_dir, "%s/open", dir3);
        mkdir(open_dir, 0777);
        chmod(open_dir, 0777);
        snprintf(svc.listen[0], sizeof svc.listen[0], "stream:%s/x.sock", open_dir);
        svc.listen_count = 1;
        assert(service_listen_open(&svc) == -1 && svc.listen_open == 0);
        snprintf(target, sizeof target, "%s/victim", dir3);
        write_svc(target, "x");
        chmod(target, 0600);
        snprintf(svc.listen[0], sizeof svc.listen[0], "fifo:%s/sub/link.fifo", dir3);
        snprintf(p, sizeof p, "%s/sub/link.fifo", dir3);
        assert(symlink(target, p) == 0);
        assert(service_listen_open(&svc) == -1);
        struct stat vst;
        assert(stat(target, &vst) == 0 && (vst.st_mode & 07777) == 0600);
    }

    {
        const char *cl = "BOOT_IMAGE=/vmlinuz ro quiet audit=0 rd.live.image";
        assert(cmdline_word_match(cl, "quiet") && cmdline_word_match(cl, "audit"));
        assert(cmdline_word_match(cl, "audit=0") && !cmdline_word_match(cl, "audit=off"));
        assert(cmdline_word_match(cl, "rd.live.image") && !cmdline_word_match(cl, "rd.live"));
        assert(!cmdline_word_match(cl, "qui") && !cmdline_word_match("", "x"));

        char d[200], f[200], e[200], c[300];
        snprintf(d, sizeof d, "%s/cdir", dir3);
        snprintf(e, sizeof e, "%s/cempty", dir3);
        snprintf(f, sizeof f, "%s/cdir/file", dir3);
        mkdir(d, 0755);
        mkdir(e, 0755);
        write_svc(f, "data");
        chmod(f, 0755);
        struct { const char *fmt; const char *arg; int want; } cases[] = {
            { "path_exists:%s", f, 1 }, { "path_exists:!%s", f, 0 },
            { "path_exists:%s.nope", f, 0 }, { "path_exists:!%s.nope", f, 1 },
            { "path_exists_glob:%s/*", d, 1 }, { "path_exists_glob:%s/*", e, 0 },
            { "path_is_dir:%s", d, 1 }, { "path_is_dir:%s", f, 0 },
            { "dir_not_empty:%s", d, 1 }, { "dir_not_empty:%s", e, 0 },
            { "file_not_empty:%s", f, 1 }, { "file_not_empty:%s", d, 0 },
            { "file_is_exec:%s", f, 1 }, { "path_is_symlink:%s", f, 0 },
            { "path_is_mount:%s", "/proc", 1 }, { "path_is_mount:%s", d, 0 },
            { "path_is_rw:%s", d, 1 }, { "path_is_rw:%s.nope", d, 0 },
            { "kernel_cmdline:!%s", "no.such.cmdline.word", 1 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            snprintf(c, sizeof c, cases[i].fmt, cases[i].arg);
            assert(service_condition_check(c) == cases[i].want);
        }
        assert(service_condition_check("bogus:/x") == -1);
        int ac = service_condition_check("ac_power:true");
        assert(ac == 0 || ac == 1);
        assert(service_condition_check("ac_power:false") == !ac);

        snprintf(p, sizeof p, "%s/cond.svc", dir3);
        snprintf(c, sizeof c,
            "name=cond\nexec=/bin/true\ncondition=path_exists:%s\n"
            "condition=dir_not_empty:|%s\ncondition=path_exists:|!%s\n", f, e, f);
        write_svc(p, c);
        char why[160];
        assert(service_load_one(p, &svc) == 0 && svc.cond_count == 3);
        assert(!service_conditions_met(&svc, why, sizeof why) && strstr(why, "none of the 2"));
        snprintf(svc.cond[1], sizeof svc.cond[1], "dir_not_empty:|%s", d);
        assert(service_conditions_met(&svc, why, sizeof why));
        snprintf(svc.cond[0], sizeof svc.cond[0], "path_exists:!%s", f);
        assert(!service_conditions_met(&svc, why, sizeof why) && strstr(why, "path_exists:!"));
        svc.cond_count = 0;
        assert(service_conditions_met(&svc, why, sizeof why));
        const char *bad[] = { "condition=path_exists\n", "condition=nope:/x\n", "condition=path_exists:\n",
                              "condition=path_exists:|\n", "condition=path_exists:|!\n", NULL };
        for (int i = 0; bad[i]; i++) {
            snprintf(c, sizeof c, "name=cond\nexec=/bin/true\n%s", bad[i]);
            write_svc(p, c);
            assert(service_load_one(p, &svc) == -1);
        }
    }

    printf("all service env tests passed\n");
    return 0;
}
