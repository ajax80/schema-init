#include "../ctl_colour.h"
#include <assert.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *run(const char *in) {
    char *buf = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    status_colourize(in, f);
    fclose(f);
    return buf;
}

int main(void) {
    char *o = run("  a  pid=1  state=PERFECT  restarts=0\n");
    assert(!strcmp(o, "  a  pid=1  state=\033[35mPERFECT\033[0m  restarts=0\n"));
    free(o);
    /* a name that itself reads state=x, at line start with no space: left alone */
    o = run("state=x  pid=1  state=EXCISED\n");
    assert(!strcmp(o, "state=x  pid=1  state=\033[31mEXCISED\033[0m\n"));
    free(o);
    o = run("  b  state=BOGUS\n");
    assert(!strcmp(o, "  b  state=BOGUS\n"));
    free(o);
    o = run("  c  state=PERFECTLY\n");
    assert(!strcmp(o, "  c  state=PERFECTLY\n"));
    free(o);
    o = run("");
    assert(!strcmp(o, ""));
    free(o);

    int m, s;
    assert(openpty(&m, &s, NULL, NULL, NULL) == 0);
    setenv("TERM", "xterm", 1);
    unsetenv("NO_COLOR");
    assert(colour_wanted(s) == 1);
    setenv("NO_COLOR", "1", 1);
    assert(colour_wanted(s) == 0);
    unsetenv("NO_COLOR");
    setenv("TERM", "dumb", 1);
    assert(colour_wanted(s) == 0);
    unsetenv("TERM");
    assert(colour_wanted(s) == 0);
    setenv("TERM", "xterm", 1);
    int p[2];
    assert(pipe(p) == 0);
    assert(colour_wanted(p[1]) == 0);
    puts("test_ctl_colour: ok");
    return 0;
}
