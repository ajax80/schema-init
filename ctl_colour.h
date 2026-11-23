#ifndef CTL_COLOUR_H
#define CTL_COLOUR_H
/* schema-ctl status on a terminal: colour each state with the board's palette.
 * PID 1 sends plain text; colouring here keeps --json/--kv and pipes clean. */
#include "state_colors.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline int colour_wanted(int fd) {
    const char *t = getenv("TERM");
    return isatty(fd) && !getenv("NO_COLOR") && t && *t && strcmp(t, "dumb");
}

/* Only a known state word right after " state=" and ending at whitespace is
 * coloured: service names come from file names and could read "state=x". */
static inline void status_colourize(const char *in, FILE *out) {
    const char *p = in;
    while (*p) {
        const char *hit = strstr(p, " state=");
        if (!hit) break;
        const char *w = hit + 7, *e = w;
        while (*e == '_' || isupper((unsigned char)*e)) e++;
        int st = (*e == '\0' || isspace((unsigned char)*e)) ? state_from_name(w, (size_t)(e - w)) : -1;
        if (st < 0) {
            fwrite(p, 1, (size_t)(w - p), out);
            p = w;
            continue;
        }
        fwrite(p, 1, (size_t)(w - p), out);
        fprintf(out, "%s%.*s\033[0m", state_ansi((uint8_t)st), (int)(e - w), w);
        p = e;
    }
    fputs(p, out);
}
#endif
