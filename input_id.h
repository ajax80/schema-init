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

#endif
