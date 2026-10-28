#ifndef SCHEMA_INPUT_ID_H
#define SCHEMA_INPUT_ID_H

#include "path_id.h"   /* transitively: schema-udev.h (struct uevent, safe_copy) + pi_* helpers */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* --- input-event-codes.h subset (kernel headers not included) --- */
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_SW  0x05
#define EV_SND 0x12
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

/* sysfs capability masks are space-separated words of the KERNEL's long, MSW
 * first: 64-bit on a 64-bit kernel even under a 32-bit userland (armhf on a
 * Pi 3/4/5), 32-bit on a 32-bit kernel. Words are stored as uint64_t and every
 * mask helper takes that width, word_bits (32 or 64). */
#define IID_NWORDS 24   /* 24*32 = 768 bits >= KEY_MAX+1 (0x300) */

/* Kernel word width for the call site. Not inferred from token width: the
 * kernel prints each word with unpadded %lx, so "1f0000" fits either width. */
static inline int iid_kernel_word_bits(void) {
    struct utsname u;
    if (uname(&u) != 0) return (int)sizeof(long) * 8;
    return strstr(u.machine, "64") ? 64 : 32;
}

static inline void iid_parse_mask(const char *s, uint64_t arr[IID_NWORDS], unsigned word_bits) {
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
        if (w >= 0 && w < IID_NWORDS)
            arr[w] = strtoull(t, NULL, 16) & (word_bits == 64 ? UINT64_MAX : UINT32_MAX);
    }
}

static inline int iid_test_bit(const uint64_t arr[IID_NWORDS], unsigned n, unsigned word_bits) {
    unsigned w = n / word_bits, b = n % word_bits;   /* b < word_bits: no oversized shift */
    if (w >= IID_NWORDS) return 0;
    return (int)((arr[w] >> b) & 1u);
}

static inline int iid_any_bit(const uint64_t arr[IID_NWORDS], unsigned lo, unsigned hi, unsigned word_bits) {
    for (unsigned b = lo; b < hi; b++)
        if (iid_test_bit(arr, b, word_bits)) return 1;
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

static inline int iid_read_masks(const char *inputdir, unsigned wb,
                                 uint64_t ev[IID_NWORDS], uint64_t key[IID_NWORDS],
                                 uint64_t rel[IID_NWORDS], uint64_t abs_[IID_NWORDS],
                                 uint64_t prop[IID_NWORDS]) {
    char b[1024];
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/ev",  b, sizeof b) == 0 ? b : NULL, ev, wb);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/key", b, sizeof b) == 0 ? b : NULL, key, wb);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/rel", b, sizeof b) == 0 ? b : NULL, rel, wb);
    iid_parse_mask(pi_sysattr(inputdir, "capabilities/abs", b, sizeof b) == 0 ? b : NULL, abs_, wb);
    iid_parse_mask(pi_sysattr(inputdir, "properties",       b, sizeof b) == 0 ? b : NULL, prop, wb);
    return 0;
}

static inline int iid_test_key(const uint64_t ev[IID_NWORDS],
                               const uint64_t key[IID_NWORDS], unsigned wb, struct uevent *out) {
    if (!iid_test_bit(ev, EV_KEY, wb)) return 0;
    int found = iid_any_bit(key, 0, BTN_MISC, wb);   /* KEY_* below BTN_* */
    int ret = 0;
    /* bits 1..31 all set; word 0 holds bits 0..31 at either width */
    if ((key[0] & 0xFFFFFFFEULL) == 0xFFFFFFFEULL) { iid_emit(out, "ID_INPUT_KEYBOARD", "1"); ret = 1; }
    if (found) { iid_emit(out, "ID_INPUT_KEY", "1"); ret = 1; }
    return ret;
}

static inline int iid_test_pointers(const uint64_t ev[IID_NWORDS], const uint64_t abs_[IID_NWORDS],
                                    const uint64_t key[IID_NWORDS], const uint64_t rel[IID_NWORDS],
                                    const uint64_t prop[IID_NWORDS], unsigned wb, struct uevent *out) {
    int has_keys = iid_test_bit(ev, EV_KEY, wb);
    int has_abs = iid_test_bit(abs_, ABS_X, wb) && iid_test_bit(abs_, ABS_Y, wb);
    int has_3d  = has_abs && iid_test_bit(abs_, ABS_Z, wb);
    int is_accel = iid_test_bit(prop, INPUT_PROP_ACCELEROMETER, wb);
    if (!has_keys && has_3d) is_accel = 1;
    if (is_accel) { iid_emit(out, "ID_INPUT_ACCELEROMETER", "1"); return 1; }

    int is_pointing_stick = iid_test_bit(prop, INPUT_PROP_POINTING_STICK, wb);
    int has_stylus = iid_test_bit(key, BTN_STYLUS, wb);
    int has_pen    = iid_test_bit(key, BTN_TOOL_PEN, wb);
    int finger_but_no_pen = iid_test_bit(key, BTN_TOOL_FINGER, wb) && !has_pen;
    int has_mouse_button = iid_any_bit(key, BTN_MOUSE, BTN_JOYSTICK, wb);
    int has_rel = iid_test_bit(ev, EV_REL, wb) && iid_test_bit(rel, REL_X, wb) && iid_test_bit(rel, REL_Y, wb);
    int has_mt  = iid_test_bit(abs_, ABS_MT_POSITION_X, wb) && iid_test_bit(abs_, ABS_MT_POSITION_Y, wb);
    int is_direct = iid_test_bit(prop, INPUT_PROP_DIRECT, wb);
    int has_touch = iid_test_bit(key, BTN_TOUCH, wb);
    int has_joy = iid_any_bit(key, BTN_JOYSTICK, BTN_DIGI, wb)
               || iid_any_bit(key, BTN_TRIGGER_HAPPY, BTN_TRIGGER_HAPPY40 + 1, wb)
               || iid_any_bit(abs_, ABS_RX, ABS_PRESSURE, wb);

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

static inline int input_id_build(const char *sysroot, const char *devpath, unsigned wb, struct uevent *out) {
    out->n = 0;
    char inp[PATH_MAX];
    if (iid_find_input_node(sysroot, devpath, inp, sizeof inp) != 0) return -1;

    uint64_t ev[IID_NWORDS], key[IID_NWORDS], rel[IID_NWORDS], abs_[IID_NWORDS], prop[IID_NWORDS];
    iid_read_masks(inp, wb, ev, key, rel, abs_, prop);

    if (iid_test_bit(ev, EV_SW, wb)) iid_emit(out, "ID_INPUT_SWITCH", "1");

    int is_pointer = iid_test_pointers(ev, abs_, key, rel, prop, wb, out);
    int is_key = iid_test_key(ev, key, wb, out);

    if (!is_pointer && !is_key && iid_test_bit(ev, EV_REL, wb)
        && (iid_test_bit(rel, REL_WHEEL, wb) || iid_test_bit(rel, REL_HWHEEL, wb))) {
        iid_emit(out, "ID_INPUT_KEY", "1");
        is_key = 1;
    }

    if (is_pointer || is_key || iid_test_bit(ev, EV_SW, wb) || iid_test_bit(ev, EV_SND, wb))
        iid_emit(out, "ID_INPUT", "1");

    return 0;
}

#endif
