#include "../verify_diff.h"
#include <stdio.h>
#include <stdlib.h>

/* the 11-18 regression driver: diff_baseline over two --check outputs, one finding per line */
static char *slurp(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(2); }
    char *b = calloc(1, 1 << 20);
    fread(b, 1, (1 << 20) - 1, fp);
    fclose(fp);
    return b;
}

int main(int argc, char **argv) {
    static const char *const kind[] = { "FAIL", "CHANGE", "WARN", "NOTE" };
    static finding_t f[256];
    char hash[65];
    if (argc != 3) { fprintf(stderr, "usage: %s BASE NEXT\n", argv[0]); return 2; }
    int n = diff_baseline(slurp(argv[1]), slurp(argv[2]), f, 256, hash);
    if (n < 0 || n > 256) { fprintf(stderr, "diff_baseline: %d\n", n); return 2; }
    for (int i = 0; i < n; i++) printf("%s %s\n", kind[f[i].kind], f[i].text);
    return 0;
}
