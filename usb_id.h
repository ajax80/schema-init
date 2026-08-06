#ifndef USB_ID_H
#define USB_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <ctype.h>

#define USB_STR_MAX 256

static inline int usb_in_safe(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '#' || c == '+' || c == '-' || c == '.'
        || c == ':' || c == '=' || c == '@' || c == '_';
}

static inline void usb_replace_whitespace(const char *in, char *out, size_t outsz) {
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) len--;   /* trim trailing */
    size_t i = 0;
    while (i < len && isspace((unsigned char)in[i])) i++;           /* skip leading */
    size_t j = 0;
    while (i < len && j + 1 < outsz) {
        if (isspace((unsigned char)in[i])) {
            while (i < len && isspace((unsigned char)in[i])) i++;
            out[j++] = '_';
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

static inline void usb_replace_chars(char *s) {
    for (; *s; s++)
        if (!usb_in_safe((unsigned char)*s)) *s = '_';
}

static inline void usb_plain(const char *in, char *out, size_t outsz) {
    usb_replace_whitespace(in, out, outsz);
    usb_replace_chars(out);
}

static inline void usb_encode(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (usb_in_safe(*p)) {
            if (j + 1 >= outsz) break;
            out[j++] = (char)*p;
        } else {
            if (j + 4 >= outsz) break;              /* "\xNN" = 4 chars + NUL */
            j += (size_t)snprintf(out + j, outsz - j, "\\x%02x", *p);
        }
    }
    out[j] = '\0';
}

static inline int usb_find_nodes(const char *sysroot, const char *devpath,
                                 char *devdir, size_t devsz, char *ifdir, size_t ifsz) {
    char cur[PATH_MAX];
    if ((size_t)snprintf(cur, sizeof cur, "%s%s", sysroot, devpath) >= sizeof cur) return -1;
    devdir[0] = '\0'; ifdir[0] = '\0';
    char sub[128];
    for (;;) {
        if (pi_subsystem(cur, sub, sizeof sub) == 0 && strcmp(sub, "usb") == 0) {
            const char *b = pi_base(cur);
            if (strchr(b, ':')) {                 /* usb_interface */
                if (ifdir[0] == '\0') safe_copy(ifdir, cur, ifsz);
            } else {                              /* usb_device */
                safe_copy(devdir, cur, devsz);
                return 0;
            }
        }
        if (pi_parent(cur) != 0) break;
    }
    return devdir[0] ? 0 : -1;
}

static inline void usb_name_field(const char *devdir, const char *attr,
                                  const char *hexfallback,
                                  char *plain, size_t psz, char *enc, size_t esz) {
    char raw[USB_STR_MAX];
    if (pi_sysattr(devdir, attr, raw, sizeof raw) != 0 || raw[0] == '\0') {
        safe_copy(plain, hexfallback, psz);
        safe_copy(enc, hexfallback, esz);
        return;
    }
    usb_plain(raw, plain, psz);
    usb_encode(raw, enc, esz);
}

#endif /* USB_ID_H */
