#include "../service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    printf("all service env tests passed\n");
    return 0;
}
