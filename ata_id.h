#ifndef ATA_ID_H
#define ATA_ID_H

#include "usb_id.h"     /* usb_plain, usb_encode (+ transitively schema-udev.h) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void ata_str_raw(const uint8_t *buf, int w0, int wc, char *raw) {
    int j = 0;
    for (int i = 0; i < wc; i++) {
        raw[j++] = (char)buf[2 * (w0 + i) + 1];   /* high byte = first char */
        raw[j++] = (char)buf[2 * (w0 + i)];       /* low byte  = second char */
    }
    raw[j] = '\0';
}

static inline int ata_id_decode(const uint8_t *buf, struct uevent *out) {
    char raw[64];
    char serial[64], model[64], model_enc[256], rev[32];

    ata_str_raw(buf, 10, 10, raw);  usb_plain(raw, serial, sizeof serial);
    ata_str_raw(buf, 27, 20, raw);  usb_plain(raw, model, sizeof model);
                                    usb_encode(raw, model_enc, sizeof model_enc);
    ata_str_raw(buf, 23, 4, raw);   usb_plain(raw, rev, sizeof rev);

    out->n = 0;
    #define UEMIT(k, v) do { \
        if (out->n < UE_MAX_KEYS) { \
            safe_copy(out->key[out->n], (k), UE_KEY_MAX); \
            safe_copy(out->val[out->n], (v), UE_VAL_MAX); \
            out->n++; \
        } \
    } while (0)

    UEMIT("ID_ATA", "1");
    UEMIT("ID_TYPE", "disk");
    UEMIT("ID_BUS", "ata");
    if (model[0]) { UEMIT("ID_MODEL", model); UEMIT("ID_MODEL_ENC", model_enc); }
    if (rev[0]) UEMIT("ID_REVISION", rev);
    if (serial[0]) {
        UEMIT("ID_SERIAL_SHORT", serial);
        char full[128];
        if (model[0]) snprintf(full, sizeof full, "%s_%s", model, serial);
        else          safe_copy(full, serial, sizeof full);
        UEMIT("ID_SERIAL", full);
    }

    unsigned w87 = (unsigned)buf[2 * 87] | ((unsigned)buf[2 * 87 + 1] << 8);
    if (w87 & 0x0100) {
        uint64_t wwn = 0;
        for (int i = 0; i < 4; i++) {
            unsigned w = (unsigned)buf[2 * (108 + i)] | ((unsigned)buf[2 * (108 + i) + 1] << 8);
            wwn = (wwn << 16) | w;
        }
        if (wwn) {
            char s[32];
            snprintf(s, sizeof s, "0x%016llx", (unsigned long long)wwn);
            UEMIT("ID_WWN", s);
            UEMIT("ID_WWN_WITH_EXTENSION", s);
        }
    }
    #undef UEMIT
    return out->n;
}

#endif /* ATA_ID_H */
