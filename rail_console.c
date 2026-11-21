#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rail_console.h"
#include "rail_glyphs.h"

static int real_ioctl(int fd, unsigned long req, void *arg) { return ioctl(fd, req, arg); }
static rc_ioctl_fn xioctl = real_ioctl;
void rc_set_ioctl(rc_ioctl_fn f) { xioctl = f ? f : real_ioctl; }

/* whole-word match in a space/newline separated list; '#' starts a comment line */
static int has_word(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; p && *p;) {
        p += strspn(p, " \t\n");
        if (*p == '#') { p = strchr(p, '\n'); continue; }
        size_t k = strcspn(p, " \t\n");
        if (k == n && !strncmp(p, word, n)) return 1;
        p += k;
    }
    return 0;
}

int rc_styled(int fd, const char *cmdline, const char *rail_conf) {
    int mode = -1;
    if (xioctl(fd, KDGETMODE, &mode) < 0 || mode != KD_TEXT) return 0;
    return !has_word(cmdline, "schema.rail=plain") && !has_word(rail_conf, "plain");
}

int rc_font_wanted(const char *cmdline, const char *rail_conf, const char *vconsole_font, int fb_xres) {
    if (fb_xres < 1280) return 0;
    if (has_word(cmdline, "schema.rail_font=off") || has_word(rail_conf, "nofont")) return 0;
    /* the box's own font choice wins; the stock one doesn't count as a choice */
    return !vconsole_font || !vconsole_font[0] || !strcmp(vconsole_font, "eurlatgr") ||
           !strcmp(vconsole_font, "schema-12x24");
}

/* KD_FONT_OP_GET writes at a 32-row pitch, up to 512 glyphs, 32 px wide */
static unsigned char saved_font[512 * 32 * 4];
static struct unipair saved_map[8192];

static int font_set(int fd, unsigned w, unsigned h, unsigned n, unsigned char *data) {
    struct console_font_op op = { .op = KD_FONT_OP_SET, .width = w, .height = h, .charcount = n, .data = data };
    return xioctl(fd, KDFONTOP, &op);
}

static int map_set(int fd, struct unipair *e, unsigned short n) {
    struct unimapinit init = { 0 };
    struct unimapdesc d = { .entry_ct = n, .entries = e };
    if (xioctl(fd, PIO_UNIMAPCLR, &init) < 0) return -1;
    return xioctl(fd, PIO_UNIMAP, &d);
}

rc_font_result_t rc_font_load(int fd) {
    struct console_font_op get = { .op = KD_FONT_OP_GET, .width = 32, .height = 32, .charcount = 512,
                                   .data = saved_font };
    struct unimapdesc map = { .entry_ct = sizeof saved_map / sizeof *saved_map, .entries = saved_map };
    int font_changed = 0;

    if (xioctl(fd, KDFONTOP, &get) < 0 || xioctl(fd, GIO_UNIMAP, &map) < 0) return RC_FONT_SKIPPED;
    if (font_set(fd, 12, 24, 256, (unsigned char *)schema_font_glyphs) == 0) {
        font_changed = 1;
        if (map_set(fd, (struct unipair *)schema_font_unimap, (unsigned short)schema_font_unimap_len) == 0)
            return RC_FONT_OK;
    }
    /* put back what was there; a SET that failed changed nothing, but a
     * cleared or half-written map always needs its old entries */
    int bad = font_changed && font_set(fd, get.width, get.height, get.charcount, saved_font) < 0;
    if (font_changed) bad |= map_set(fd, saved_map, map.entry_ct) < 0;
    return bad ? RC_FONT_FAILED_UNRESTORED : RC_FONT_FAILED_RESTORED;
}

const unsigned char rc_schema_palette[48] = {
    0x07, 0x06, 0x0b,  0xff, 0x4d, 0x5e,  0x3d, 0xdc, 0x84,  0xff, 0xd2, 0x3f,
    0x4d, 0xa3, 0xff,  0xc4, 0x6c, 0xff,  0x3f, 0xe0, 0xff,  0xc8, 0xc8, 0xd0,
    0x5c, 0x58, 0x70,  0xff, 0x80, 0x90,  0x7d, 0xff, 0xb0,  0xff, 0xe6, 0x80,
    0x80, 0xc0, 0xff,  0xe2, 0x9b, 0xff,  0x90, 0xf0, 0xff,  0xec, 0xec, 0xf2,
};

static int read48(const char *path, unsigned char *b) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, b, 49);
    close(fd);
    return n == 48 ? 0 : -1;
}

static int write48(const char *path, const unsigned char *b) {
    char dir[256], tmp[300];
    int fd, ok;
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    ok = write(fd, b, 48) == 48;
    close(fd);
    if (!ok || rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

int rc_palette_apply(int fd, const char *orig_path) {
    unsigned char orig[48];
    int have = read48(orig_path, orig) == 0;
    if (!have && xioctl(fd, GIO_CMAP, orig) < 0) return -1;
    if (xioctl(fd, PIO_CMAP, (void *)rc_schema_palette) < 0) return -1;
    if (!have) write48(orig_path, orig);
    return 0;
}

int rc_palette_restore(const char *orig_path, const char *vt_path) {
    unsigned char orig[48];
    int fd, r;
    if (read48(orig_path, orig) < 0) return -1;
    fd = open(vt_path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    r = xioctl(fd, PIO_CMAP, orig);
    close(fd);
    return r < 0 ? -1 : 0;
}
