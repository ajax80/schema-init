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

    printf("all service env tests passed\n");
    return 0;
}
