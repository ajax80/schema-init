#ifndef SCHEMA_INPUT_ID_H
#define SCHEMA_INPUT_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- input-event-codes.h subset (kernel headers not included) --- */
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_SW  0x05
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RX 0x03
#define ABS_PRESSURE 0x18
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08
#define BTN_MISC 0x100
#define BTN_MOUSE 0x110
#define BTN_JOYSTICK 0x120
#define BTN_DIGI 0x140
#define BTN_TOOL_PEN 0x140
#define BTN_TOOL_FINGER 0x145
#define BTN_TOUCH 0x14a
#define BTN_STYLUS 0x14b
#define BTN_TRIGGER_HAPPY 0x2c0
#define BTN_TRIGGER_HAPPY40 0x2e7
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_POINTING_STICK 0x05
#define INPUT_PROP_ACCELEROMETER 0x06

#define IID_NWORDS 12   /* 12*64 = 768 bits >= KEY_MAX+1 (0x300) */

static inline void iid_parse_mask(const char *s, unsigned long arr[IID_NWORDS]) {
    for (int i = 0; i < IID_NWORDS; i++) arr[i] = 0;
    if (!s) return;
    char buf[1024], cnt[1024];
    safe_copy(buf, s, sizeof buf);
    safe_copy(cnt, s, sizeof cnt);
    int k = 0; char *sv = NULL;
    for (char *t = strtok_r(cnt, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv)) k++;
    int i = 0; sv = NULL;
    for (char *t = strtok_r(buf, " \t", &sv); t; t = strtok_r(NULL, " \t", &sv), i++) {
        int w = k - 1 - i;
        if (w >= 0 && w < IID_NWORDS) arr[w] = strtoul(t, NULL, 16);
    }
}

static inline int iid_test_bit(const unsigned long arr[IID_NWORDS], unsigned n) {
    unsigned w = n / 64, b = n % 64;
    if (w >= IID_NWORDS) return 0;
    return (arr[w] >> b) & 1UL;
}

static inline int iid_any_bit(const unsigned long arr[IID_NWORDS], unsigned lo, unsigned hi) {
    for (unsigned b = lo; b < hi; b++)
        if (iid_test_bit(arr, b)) return 1;
    return 0;
}

static inline void iid_emit(struct uevent *out, const char *k, const char *v) {
    if (out->n < UE_MAX_KEYS) {
        safe_copy(out->key[out->n], k, UE_KEY_MAX);
        safe_copy(out->val[out->n], v, UE_VAL_MAX);
        out->n++;
    }
}

static inline int iid_find_input_node(const char *sysroot, const char *devpath,
                                      char *out, size_t outsz) {
    char dir[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s%s", sysroot, devpath) >= sizeof dir) return -1;
    for (;;) {
        char probe[PATH_MAX];
        if ((size_t)snprintf(probe, sizeof probe, "%s/capabilities/ev", dir) < sizeof probe
            && access(probe, R_OK) == 0) {
            safe_copy(out, dir, outsz);
            return 0;
        }
        if (pi_parent(dir) != 0) return -1;
    }
}

static inline int iid_read_masks(const char *inputdir,
                                 unsigned long ev[IID_NWORDS], unsigned long key[IID_NWORDS],
                                 unsigned long rel[IID_NWORDS], unsigned long abs_[IID_NWORDS],
                                 unsigned long prop[IID_NWORDS]) {
    char b[1024];
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/ev",  b, sizeof b) == 0 ? b : NULL, ev);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/key", b, sizeof b) == 0 ? b : NULL, key);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/rel", b, sizeof b) == 0 ? b : NULL, rel);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/abs", b, sizeof b) == 0 ? b : NULL, abs_);
    iid_parse_mask(pi_sysattr(inputdir, "properties",       b, sizeof b) == 0 ? b : NULL, prop);
    return 0;
}

static inline int iid_test_key(const unsigned long ev[IID_NWORDS],
                               const unsigned long key[IID_NWORDS], struct uevent *out) {
    if (!iid_test_bit(ev, EV_KEY)) return 0;
    unsigned long found = 0;
    for (int i = 0; i < BTN_MISC / 64; i++) found |= key[i];   /* KEY_* below BTN_* */
    int ret = 0;
    if ((key[0] & 0xFFFFFFFEUL) == 0xFFFFFFFEUL) { iid_emit(out, "ID_INPUT_KEYBOARD", "1"); ret = 1; }
    if (found != 0) { iid_emit(out, "ID_INPUT_KEY", "1"); ret = 1; }
    return ret;
}

static inline int iid_test_pointers(const unsigned long ev[IID_NWORDS], const unsigned long abs_[IID_NWORDS],
                                    const unsigned long key[IID_NWORDS], const unsigned long rel[IID_NWORDS],
                                    const unsigned long prop[IID_NWORDS], struct uevent *out) {
    int has_keys = iid_test_bit(ev, EV_KEY);
    int has_abs = iid_test_bit(abs_, ABS_X) && iid_test_bit(abs_, ABS_Y);
    int has_3d  = has_abs && iid_test_bit(abs_, ABS_Z);
    int is_accel = iid_test_bit(prop, INPUT_PROP_ACCELEROMETER);
    if (!has_keys && has_3d) is_accel = 1;
    if (is_accel) { iid_emit(out, "ID_INPUT_ACCELEROMETER", "1"); return 1; }

    int is_pointing_stick = iid_test_bit(prop, INPUT_PROP_POINTING_STICK);
    int has_stylus = iid_test_bit(key, BTN_STYLUS);
    int has_pen    = iid_test_bit(key, BTN_TOOL_PEN);
    int finger_but_no_pen = iid_test_bit(key, BTN_TOOL_FINGER) && !has_pen;
    int has_mouse_button = iid_any_bit(key, BTN_MOUSE, BTN_JOYSTICK);
    int has_rel = iid_test_bit(ev, EV_REL) && iid_test_bit(rel, REL_X) && iid_test_bit(rel, REL_Y);
    int has_mt  = iid_test_bit(abs_, ABS_MT_POSITION_X) && iid_test_bit(abs_, ABS_MT_POSITION_Y);
    int is_direct = iid_test_bit(prop, INPUT_PROP_DIRECT);
    int has_touch = iid_test_bit(key, BTN_TOUCH);
    int has_joy = iid_any_bit(key, BTN_JOYSTICK, BTN_DIGI)
               || iid_any_bit(key, BTN_TRIGGER_HAPPY, BTN_TRIGGER_HAPPY40 + 1)
               || iid_any_bit(abs_, ABS_RX, ABS_PRESSURE);

    int is_tablet = 0, is_touchpad = 0, is_touchscreen = 0, is_joystick = 0, is_mouse = 0;
    if (has_abs) {
        if (has_stylus || has_pen) is_tablet = 1;
        else if (finger_but_no_pen && !is_direct) is_touchpad = 1;
        else if (has_mouse_button) is_mouse = 1;
        else if (has_touch || is_direct) is_touchscreen = 1;
        else if (has_joy) is_joystick = 1;
    } else if (has_joy) {
        is_joystick = 1;
    }

    if (has_mt) {
        if (is_direct) is_touchscreen = 1;
        else is_touchpad = 1;
    }

    if (!is_tablet && !is_touchpad && !is_joystick && has_mouse_button && (has_rel || !has_abs))
        is_mouse = 1;

    if (is_pointing_stick) iid_emit(out, "ID_INPUT_POINTINGSTICK", "1");
    if (is_mouse)          iid_emit(out, "ID_INPUT_MOUSE", "1");
    if (is_touchpad)       iid_emit(out, "ID_INPUT_TOUCHPAD", "1");
    if (is_touchscreen)    iid_emit(out, "ID_INPUT_TOUCHSCREEN", "1");
    if (is_joystick)       iid_emit(out, "ID_INPUT_JOYSTICK", "1");
    if (is_tablet)         iid_emit(out, "ID_INPUT_TABLET", "1");
    return is_tablet || is_mouse || is_touchpad || is_touchscreen || is_joystick || is_pointing_stick;
}

static inline int input_id_build(const char *sysroot, const char *devpath, struct uevent *out) {
    out->n = 0;
    char inp[PATH_MAX];
    if (iid_find_input_node(sysroot, devpath, inp, sizeof inp) != 0) return -1;

    unsigned long ev[IID_NWORDS], key[IID_NWORDS], rel[IID_NWORDS], abs_[IID_NWORDS], prop[IID_NWORDS];
    iid_read_masks(inp, ev, key, rel, abs_, prop);

    if (iid_test_bit(ev, EV_SW)) iid_emit(out, "ID_INPUT_SWITCH", "1");

    int is_pointer = iid_test_pointers(ev, abs_, key, rel, prop, out);
    int is_key = iid_test_key(ev, key, out);

    if (!is_pointer && !is_key && iid_test_bit(ev, EV_REL)
        && (iid_test_bit(rel, REL_WHEEL) || iid_test_bit(rel, REL_HWHEEL))) {
        iid_emit(out, "ID_INPUT_KEY", "1");
        is_key = 1;
    }

    if (is_pointer || is_key || iid_test_bit(ev, EV_SW))
        iid_emit(out, "ID_INPUT", "1");

    return 0;
}

#endif
