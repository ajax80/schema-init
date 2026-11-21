#ifndef RAIL_CONSOLE_H
#define RAIL_CONSOLE_H

/* Every VT ioctl the styled rail makes. Best-effort: nothing here may block
 * or fail a boot, and a half-changed console is always put back. */

typedef int (*rc_ioctl_fn)(int fd, unsigned long req, void *arg);
void rc_set_ioctl(rc_ioctl_fn f);   /* tests inject failures; default ioctl(2) */

/* fd is a VT in KD_TEXT and nothing switched the styling off */
int rc_styled(int fd, const char *cmdline, const char *rail_conf);
/* fb_xres -1: no /dev/fb0 yet */
int rc_font_wanted(const char *cmdline, const char *rail_conf, const char *vconsole_font, int fb_xres);

typedef enum { RC_FONT_OK, RC_FONT_SKIPPED, RC_FONT_FAILED_RESTORED, RC_FONT_FAILED_UNRESTORED } rc_font_result_t;
rc_font_result_t rc_font_load(int fd);

extern const unsigned char rc_schema_palette[48];
/* saves the pre-schema palette to orig_path on the first apply only (a
 * re-exec'd PID 1 finds it there), then loads ours. 0 ok, -1 failed */
int rc_palette_apply(int fd, const char *orig_path);
/* the saved palette back, through vt_path; -1 when there is none or it fails */
int rc_palette_restore(const char *orig_path, const char *vt_path);

#endif
