#include "../cdrom_id.h"
#include "fixtures/cdrom_getconf_sr0.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int has(const struct uevent *ev, const char *k) {
    const char *v = uevent_get(ev, k);
    return v && strcmp(v, "1") == 0;
}

int main(void) {
    struct uevent ev;
    int n = cdrom_id_decode(cdrom_getconf_sr0, sizeof cdrom_getconf_sr0, &ev);

    const char *want[] = {
        "ID_CDROM", "ID_CDROM_CD", "ID_CDROM_CD_R", "ID_CDROM_CD_RW",
        "ID_CDROM_DVD", "ID_CDROM_DVD_R", "ID_CDROM_DVD_RAM",
        "ID_CDROM_DVD_RW", "ID_CDROM_DVD_RW_RO", "ID_CDROM_DVD_RW_SEQ",
        "ID_CDROM_DVD_R_DL", "ID_CDROM_DVD_R_DL_SEQ", "ID_CDROM_DVD_R_DL_JR",
        "ID_CDROM_DVD_PLUS_R", "ID_CDROM_DVD_PLUS_RW", "ID_CDROM_DVD_PLUS_R_DL",
        "ID_CDROM_RW_REMOVABLE", NULL
    };
    int nwant = 0;
    for (int i = 0; want[i]; i++) { assert(has(&ev, want[i])); nwant++; }
    assert(nwant == 17);
    assert(n == 17);                 /* exactly 17 — no extras */
    /* no media keys this slice */
    for (int i = 0; i < ev.n; i++) assert(strncmp(ev.key[i], "ID_CDROM_MEDIA", 14) != 0);

    /* truncated blob -> 0 keys, no crash */
    struct uevent ev2;
    assert(cdrom_id_decode(cdrom_getconf_sr0, 4, &ev2) == 0);

    /* synthetic single-profile (0x08 CD-ROM): header(8) + profile-list feature */
    uint8_t b[16] = {0,0,0,12, 0,0,0,0,   /* header: datalen 12, current profile 0 */
                     0x00,0x00,0x03,0x04, /* feature 0x0000, addlen 4 */
                     0x00,0x08,0x00,0x00}; /* profile 0x08 */
    struct uevent ev3;
    cdrom_id_decode(b, sizeof b, &ev3);
    assert(has(&ev3, "ID_CDROM") && has(&ev3, "ID_CDROM_CD"));
    assert(!has(&ev3, "ID_CDROM_DVD"));

    printf("test_cdrom_id: OK\n");
    return 0;
}
