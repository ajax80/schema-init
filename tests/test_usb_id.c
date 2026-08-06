#include "../usb_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* --- fake-sysfs builders (same idiom as test_path_id.c) --- */
static void mkdirp(const char *path) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    assert(system(cmd) == 0);
}
static void mkfile(const char *path, const char *content) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}
static void mklink(const char *linkpath, const char *target) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", linkpath);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirp(dir); }
    unlink(linkpath);
    assert(symlink(target, linkpath) == 0);
}

static void test_encoders(void) {
    char out[USB_STR_MAX];

    usb_plain("USB OPTICAL MOUSE ", out, sizeof out);
    assert(strcmp(out, "USB_OPTICAL_MOUSE") == 0);

    usb_plain("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic_Technology_Co.__Ltd.") == 0);

    usb_plain("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion") == 0);

    usb_encode("GenesysLogic Technology Co., Ltd.", out, sizeof out);
    assert(strcmp(out, "GenesysLogic\\x20Technology\\x20Co.\\x2c\\x20Ltd.") == 0);

    usb_encode("Seagate ", out, sizeof out);
    assert(strcmp(out, "Seagate\\x20") == 0);

    usb_encode("Expansion       ", out, sizeof out);
    assert(strcmp(out, "Expansion\\x20\\x20\\x20\\x20\\x20\\x20\\x20") == 0);

    /* SAFE chars survive both forms */
    usb_plain("7.0.12-cachyos1 x86_64", out, sizeof out);
    assert(strcmp(out, "7.0.12-cachyos1_x86_64") == 0);
    usb_encode("a:b-c.d", out, sizeof out);
    assert(strcmp(out, "a:b-c.d") == 0);

    printf("test_encoders OK\n");
}

int main(void) {
    test_encoders();
    printf("ALL usb_id tests passed\n");
    return 0;
}
