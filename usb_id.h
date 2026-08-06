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

static inline const char *usb_type_from_iface(const char *ifdir) {
    char cls[16], sub[16];
    if (pi_sysattr(ifdir, "bInterfaceClass", cls, sizeof cls) != 0) return "generic";
    if (strcmp(cls, "01") == 0) return "audio";
    if (strcmp(cls, "03") == 0) return "hid";
    if (strcmp(cls, "06") == 0) return "media";
    if (strcmp(cls, "07") == 0) return "printer";
    if (strcmp(cls, "08") == 0) {
        if (pi_sysattr(ifdir, "bInterfaceSubClass", sub, sizeof sub) == 0) {
            if (strcmp(sub, "02") == 0) return "cd";
            if (strcmp(sub, "03") == 0) return "tape";
            if (strcmp(sub, "04") == 0 || strcmp(sub, "07") == 0) return "floppy";
        }
        return "disk";
    }
    if (strcmp(cls, "09") == 0) return "hub";
    if (strcmp(cls, "0e") == 0) return "video";
    if (strcmp(cls, "e0") == 0) return "wireless";
    return "generic";
}

static inline int usb_driver(const char *ifdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/driver", ifdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}

struct usb_if { int num; char trip[16]; };

static inline void usb_interfaces(const char *devdir, char *out, size_t outsz) {
    const char *devbase = pi_base(devdir);
    char prefix[128];
    snprintf(prefix, sizeof prefix, "%s:", devbase);
    size_t plen = strlen(prefix);

    struct usb_if ifs[32];
    int n = 0;
    DIR *d = opendir(devdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 32) {
            if (strncmp(e->d_name, prefix, plen) != 0) continue;
            char ifp[PATH_MAX];
            if ((size_t)snprintf(ifp, sizeof ifp, "%s/%s", devdir, e->d_name) >= sizeof ifp) continue;
            char cls[16], sub[16], pro[16], num[16];
            if (pi_sysattr(ifp, "bInterfaceClass", cls, sizeof cls) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceSubClass", sub, sizeof sub) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceProtocol", pro, sizeof pro) != 0) continue;
            if (pi_sysattr(ifp, "bInterfaceNumber", num, sizeof num) != 0) continue;
            ifs[n].num = (int)strtol(num, NULL, 16);
            if ((size_t)snprintf(ifs[n].trip, sizeof ifs[n].trip, "%s%s%s", cls, sub, pro) >= sizeof ifs[n].trip) continue;
            n++;
        }
        closedir(d);
    }
    /* insertion sort ascending by bInterfaceNumber */
    for (int i = 1; i < n; i++) {
        struct usb_if key = ifs[i];
        int j = i - 1;
        while (j >= 0 && ifs[j].num > key.num) { ifs[j + 1] = ifs[j]; j--; }
        ifs[j + 1] = key;
    }
    /* build ":t:t:...:" with dedup */
    char buf[USB_STR_MAX];
    size_t bl = 0;
    buf[bl++] = ':'; buf[bl] = '\0';
    for (int i = 0; i < n; i++) {
        char probe[16];
        snprintf(probe, sizeof probe, ":%s:", ifs[i].trip);
        if (strstr(buf, probe)) continue;              /* dedup: triplet already present */
        char seg[16];
        int w = snprintf(seg, sizeof seg, "%s:", ifs[i].trip);
        if (w > 0 && bl + (size_t)w < sizeof buf) {
            memcpy(buf + bl, seg, (size_t)w);
            bl += (size_t)w;
            buf[bl] = '\0';
        }
    }
    safe_copy(out, buf, outsz);
}

static inline int usb_id_build(const char *sysroot, const char *devpath, struct uevent *out) {
    char devdir[PATH_MAX], ifdir[PATH_MAX];
    if (usb_find_nodes(sysroot, devpath, devdir, sizeof devdir, ifdir, sizeof ifdir) != 0)
        return -1;

    char vid[16], pid[16], rev[16];
    if (pi_sysattr(devdir, "idVendor", vid, sizeof vid) != 0) return -1;   /* not USB */
    if (pi_sysattr(devdir, "idProduct", pid, sizeof pid) != 0) return -1;
    if (pi_sysattr(devdir, "bcdDevice", rev, sizeof rev) != 0) rev[0] = '\0';

    char vendor[USB_STR_MAX], vendor_enc[USB_STR_MAX];
    char model[USB_STR_MAX], model_enc[USB_STR_MAX];
    usb_name_field(devdir, "manufacturer", vid, vendor, sizeof vendor, vendor_enc, sizeof vendor_enc);
    usb_name_field(devdir, "product", pid, model, sizeof model, model_enc, sizeof model_enc);

    char serial_short[USB_STR_MAX];
    int have_serial = 0;
    {
        char raw[USB_STR_MAX];
        if (pi_sysattr(devdir, "serial", raw, sizeof raw) == 0 && raw[0]) {
            usb_plain(raw, serial_short, sizeof serial_short);
            have_serial = 1;
        }
    }

    char serial[USB_STR_MAX * 3];
    if (have_serial) snprintf(serial, sizeof serial, "%s_%s_%s", vendor, model, serial_short);
    else             snprintf(serial, sizeof serial, "%s_%s", vendor, model);

    const char *type = ifdir[0] ? usb_type_from_iface(ifdir) : "generic";
    char ifaces[USB_STR_MAX];
    usb_interfaces(devdir, ifaces, sizeof ifaces);
    char ifnum[16];
    int have_ifnum = ifdir[0] && pi_sysattr(ifdir, "bInterfaceNumber", ifnum, sizeof ifnum) == 0;
    char drv[64];
    int have_drv = ifdir[0] && usb_driver(ifdir, drv, sizeof drv) == 0;

    out->n = 0;
    #define UEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    UEMIT("ID_BUS", "usb");
    UEMIT("ID_MODEL", model); UEMIT("ID_MODEL_ENC", model_enc); UEMIT("ID_MODEL_ID", pid);
    UEMIT("ID_SERIAL", serial);
    if (have_serial) UEMIT("ID_SERIAL_SHORT", serial_short);
    UEMIT("ID_VENDOR", vendor); UEMIT("ID_VENDOR_ENC", vendor_enc); UEMIT("ID_VENDOR_ID", vid);
    if (rev[0]) UEMIT("ID_REVISION", rev);
    UEMIT("ID_TYPE", type);

    UEMIT("ID_USB_MODEL", model); UEMIT("ID_USB_MODEL_ENC", model_enc); UEMIT("ID_USB_MODEL_ID", pid);
    UEMIT("ID_USB_SERIAL", serial);
    if (have_serial) UEMIT("ID_USB_SERIAL_SHORT", serial_short);
    UEMIT("ID_USB_VENDOR", vendor); UEMIT("ID_USB_VENDOR_ENC", vendor_enc); UEMIT("ID_USB_VENDOR_ID", vid);
    if (rev[0]) UEMIT("ID_USB_REVISION", rev);
    UEMIT("ID_USB_TYPE", type);
    UEMIT("ID_USB_INTERFACES", ifaces);
    if (have_ifnum) UEMIT("ID_USB_INTERFACE_NUM", ifnum);
    if (have_drv) UEMIT("ID_USB_DRIVER", drv);
    #undef UEMIT
    return 0;
}

#endif /* USB_ID_H */
