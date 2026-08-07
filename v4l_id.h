#ifndef V4L_ID_H
#define V4L_ID_H

#include "schema-udev.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static inline int v4l_id_decode(const struct v4l2_capability *cap, struct uevent *out) {
    out->n = 0;
    #define VEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    VEMIT("ID_V4L_VERSION", "2");

    char product[UE_VAL_MAX];
    snprintf(product, sizeof product, "%.32s", (const char *)cap->card);  /* card is 32 bytes */
    VEMIT("ID_V4L_PRODUCT", product);

    unsigned caps = (cap->capabilities & V4L2_CAP_DEVICE_CAPS) ? cap->device_caps
                                                              : cap->capabilities;
    char c[64] = ":";
    if (caps & V4L2_CAP_VIDEO_CAPTURE) safe_copy(c + strlen(c), "capture:", sizeof c - strlen(c));
    if (caps & V4L2_CAP_VIDEO_OUTPUT)  safe_copy(c + strlen(c), "output:",  sizeof c - strlen(c));
    if (caps & V4L2_CAP_VIDEO_OVERLAY) safe_copy(c + strlen(c), "overlay:", sizeof c - strlen(c));
    VEMIT("ID_V4L_CAPABILITIES", c);

    #undef VEMIT
    return out->n;
}

static inline int v4l_id_query(const char *devnode, struct v4l2_capability *cap) {
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ioctl(fd, VIDIOC_QUERYCAP, cap);
    close(fd);
    return rc < 0 ? -1 : 0;
}

static inline int v4l_id_build(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *out) {
    (void)sysroot; (void)devpath;
    out->n = 0;
    if (!devnode) return 0;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (v4l_id_query(devnode, &cap) != 0) return 0;
    return v4l_id_decode(&cap, out);
}

#endif /* V4L_ID_H */
