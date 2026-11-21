#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../rail_console.h"

/* a fake VT: records each request; fails the first `fail_n` calls matching
 * (fail_req, fail_op) where fail_op is the KDFONTOP op or -1 for any */
static struct { unsigned long req; int op; } calls[64];
static int ncalls, mode_reply = KD_TEXT, fail_n, fail_op = -1;
static unsigned long fail_req;
static unsigned char cmap_now[48];

static int fake(int fd, unsigned long req, void *arg) {
    (void)fd;
    int op = req == KDFONTOP ? (int)((struct console_font_op *)arg)->op : -1;
    if (ncalls < 64) { calls[ncalls].req = req; calls[ncalls].op = op; ncalls++; }
    if (fail_n && req == fail_req && (fail_op < 0 || op == fail_op)) { fail_n--; errno = EIO; return -1; }
    switch (req) {
    case KDGETMODE: *(int *)arg = mode_reply; return 0;
    case KDFONTOP:
        if (op == KD_FONT_OP_GET) {
            struct console_font_op *f = arg;
            f->width = 8; f->height = 16; f->charcount = 256;
            if (f->data) memset(f->data, 0x5a, 256 * 32);
        }
        return 0;
    case GIO_UNIMAP: {
        struct unimapdesc *u = arg;
        if (u->entries) { u->entries[0].unicode = 0x41; u->entries[0].fontpos = 65; }
        u->entry_ct = 1;
        return 0;
    }
    case GIO_CMAP: memset(arg, 0x11, 48); return 0;
    case PIO_CMAP: memcpy(cmap_now, arg, 48); return 0;
    default: return 0;
    }
}

static void reset(void) { ncalls = 0; fail_n = 0; fail_op = -1; fail_req = 0; mode_reply = KD_TEXT; }
static int called(unsigned long req, int op) {
    int n = 0;
    for (int i = 0; i < ncalls; i++) n += calls[i].req == req && (op < 0 || calls[i].op == op);
    return n;
}

static void styled_rules(void) {
    reset(); fail_req = KDGETMODE; fail_n = 1;
    assert(rc_styled(1, "quiet", "") == 0);
    reset(); mode_reply = KD_GRAPHICS;
    assert(rc_styled(1, "quiet", "") == 0);
    reset();
    assert(rc_styled(1, "root=/dev/x quiet", "") == 1);
    assert(rc_styled(1, "root=/dev/x schema.rail=plain quiet", "") == 0);
    assert(rc_styled(1, "xschema.rail=plain", "") == 1);
    assert(rc_styled(1, "quiet", "plain\n") == 0);
    assert(rc_styled(1, "quiet", "# plain\nnofont\n") == 1);
}

static void font_wanted_rules(void) {
    assert(rc_font_wanted("", "", "", -1) == 0);
    assert(rc_font_wanted("", "", "", 1279) == 0);
    assert(rc_font_wanted("", "", "", 1280) == 1);
    assert(rc_font_wanted("", "", "ter-v32b", 1920) == 0);
    assert(rc_font_wanted("", "", "eurlatgr", 1920) == 1);
    assert(rc_font_wanted("", "", "schema-12x24", 1920) == 1);
    assert(rc_font_wanted("", "nofont\n", "", 1920) == 0);
    assert(rc_font_wanted("a schema.rail_font=off b", "", "", 1920) == 0);
}

static void font_fail_each_step(void) {
    reset();
    assert(rc_font_load(3) == RC_FONT_OK);
    assert(called(KDFONTOP, KD_FONT_OP_SET) == 1 && called(PIO_UNIMAPCLR, -1) == 1 && called(PIO_UNIMAP, -1) == 1);

    reset(); fail_req = KDFONTOP; fail_op = KD_FONT_OP_GET; fail_n = 1;
    assert(rc_font_load(3) == RC_FONT_SKIPPED);
    assert(called(KDFONTOP, KD_FONT_OP_SET) == 0);

    reset(); fail_req = GIO_UNIMAP; fail_n = 1;
    assert(rc_font_load(3) == RC_FONT_SKIPPED);
    assert(called(KDFONTOP, KD_FONT_OP_SET) == 0);

    reset(); fail_req = KDFONTOP; fail_op = KD_FONT_OP_SET; fail_n = 1;
    assert(rc_font_load(3) == RC_FONT_FAILED_RESTORED);
    assert(called(PIO_UNIMAPCLR, -1) == 0);

    reset(); fail_req = PIO_UNIMAPCLR; fail_n = 1;
    assert(rc_font_load(3) == RC_FONT_FAILED_RESTORED);
    assert(called(KDFONTOP, KD_FONT_OP_SET) == 2);   /* ours, then the saved one back */

    reset(); fail_req = PIO_UNIMAP; fail_n = 1;
    assert(rc_font_load(3) == RC_FONT_FAILED_RESTORED);
    assert(called(KDFONTOP, KD_FONT_OP_SET) == 2 && called(PIO_UNIMAPCLR, -1) == 2 && called(PIO_UNIMAP, -1) == 2);

    reset(); fail_req = PIO_UNIMAP; fail_n = 2;          /* the restore of the map fails too */
    assert(rc_font_load(3) == RC_FONT_FAILED_UNRESTORED);
}

static void palette(void) {
    char dir[] = "/tmp/rcpalXXXXXX", path[128], vt[128];
    unsigned char b[64];
    int fd;
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/sub/console-orig", dir);
    snprintf(vt, sizeof vt, "%s/vt", dir);

    reset(); fail_req = PIO_CMAP; fail_n = 1;
    assert(rc_palette_apply(3, path) == -1);
    assert(access(path, F_OK) == -1);   /* nothing applied: nothing to restore */

    reset();
    assert(rc_palette_apply(3, path) == 0);
    assert(called(GIO_CMAP, -1) == 1 && !memcmp(cmap_now, rc_schema_palette, 48));
    fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, b, sizeof b) == 48);
    close(fd);
    for (int i = 0; i < 48; i++) assert(b[i] == 0x11);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* the re-exec'd PID 1: the original is already saved, never saved again */
    reset();
    assert(rc_palette_apply(3, path) == 0);
    assert(called(GIO_CMAP, -1) == 0 && called(PIO_CMAP, -1) == 1);
    fd = open(path, O_RDONLY);
    assert(read(fd, b, sizeof b) == 48 && b[0] == 0x11);
    close(fd);

    reset();
    fd = open(vt, O_CREAT | O_WRONLY, 0600);
    close(fd);
    memset(cmap_now, 0, sizeof cmap_now);
    assert(rc_palette_restore(path, vt) == 0);
    for (int i = 0; i < 48; i++) assert(cmap_now[i] == 0x11);

    reset();
    unlink(path);
    assert(rc_palette_restore(path, vt) == -1 && ncalls == 0);
}

int main(void) {
    rc_set_ioctl(fake);
    styled_rules();
    font_wanted_rules();
    font_fail_each_step();
    palette();
    printf("test_rail_console: ok\n");
    return 0;
}
