#include "../v4l_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *get(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

static void cap_init(struct v4l2_capability *c, const char *card,
                     unsigned caps, unsigned dcaps) {
    memset(c, 0, sizeof *c);
    snprintf((char *)c->card, sizeof c->card, "%s", card);
    c->capabilities = caps;
    c->device_caps = dcaps;
}

int main(void) {
    struct v4l2_capability c;
    struct uevent ev;

    /* capture node (uses device_caps because DEVICE_CAPS set) */
    cap_init(&c, "USB2.0 UVC PC Camera: USB2.0 UV",
             V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_DEVICE_CAPS, V4L2_CAP_VIDEO_CAPTURE);
    assert(v4l_id_decode(&c, &ev) == 3);
    assert(strcmp(get(&ev, "ID_V4L_VERSION"), "2") == 0);
    assert(strcmp(get(&ev, "ID_V4L_PRODUCT"), "USB2.0 UVC PC Camera: USB2.0 UV") == 0);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":capture:") == 0);

    /* metadata node: no capture/output/overlay in device_caps -> ":" */
    cap_init(&c, "meta", V4L2_CAP_META_CAPTURE | V4L2_CAP_DEVICE_CAPS, V4L2_CAP_META_CAPTURE);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":") == 0);

    /* DEVICE_CAPS selection: capabilities has CAPTURE but device_caps does not */
    cap_init(&c, "sel", V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_DEVICE_CAPS, 0);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":") == 0);   /* reflects device_caps */

    /* no DEVICE_CAPS bit -> falls back to capabilities */
    cap_init(&c, "old", V4L2_CAP_VIDEO_CAPTURE, 0);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":capture:") == 0);

    /* output + overlay */
    cap_init(&c, "out", V4L2_CAP_DEVICE_CAPS, V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OVERLAY);
    v4l_id_decode(&c, &ev);
    assert(strcmp(get(&ev, "ID_V4L_CAPABILITIES"), ":output:overlay:") == 0);

    printf("test_v4l_id: OK\n");
    return 0;
}
