#include "karg.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

int karg_split(const char *opts, char (*out)[512], int max) {
    int n = 0;
    for (const char *p = opts;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) return n;
        if (n == max) return -1;
        int quoted = *p == '"', inq = quoted;
        if (quoted) p++;
        size_t i, eq = 0;
        for (i = 0; p[i] && (inq || !isspace((unsigned char)p[i])); i++) {
            if (!eq && p[i] == '=') eq = i + 1;
            if (p[i] == '"') inq = !inq;
        }
        /* next_arg(): a value's opening quote goes, and the word's last char if it is a quote */
        char w[4096];
        size_t wl = 0;
        for (size_t j = 0; j < i && wl < sizeof w - 1; j++) {
            if (eq && j == eq && p[j] == '"') continue;
            if (j == i - 1 && p[j] == '"' && (quoted || (eq && p[eq] == '"' && j > eq))) continue;
            w[wl++] = p[j];
        }
        snprintf(out[n++], 512, "%.*s", (int)wl, w);
        p += i;
    }
}
