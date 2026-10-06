#include "../ata_id.h"
#include "fixtures/ata_sda_identify.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *get(const struct uevent *ev, const char *k) { return uevent_get(ev, k); }

int main(void) {
    struct uevent ev;
    int n = ata_id_decode(ata_sda_identify, 0, &ev);
    assert(n > 0);
    assert(strcmp(get(&ev, "ID_ATA"), "1") == 0);
    assert(strcmp(get(&ev, "ID_BUS"), "ata") == 0);
    assert(strcmp(get(&ev, "ID_TYPE"), "disk") == 0);
    assert(strcmp(get(&ev, "ID_SERIAL_SHORT"), "WD-WCC6Y2RF681K") == 0);
    assert(strcmp(get(&ev, "ID_MODEL"), "WDC_WD10EZEX-08WN4A0") == 0);
    assert(strcmp(get(&ev, "ID_MODEL_ENC"),
        "WDC\\x20WD10EZEX-08WN4A0\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20"
        "\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20\\x20") == 0);
    assert(strcmp(get(&ev, "ID_REVISION"), "02.01A02") == 0);
    assert(strcmp(get(&ev, "ID_SERIAL"), "WDC_WD10EZEX-08WN4A0_WD-WCC6Y2RF681K") == 0);
    assert(strcmp(get(&ev, "ID_WWN"), "0x50014ee211e8fd40") == 0);
    assert(strcmp(get(&ev, "ID_WWN_WITH_EXTENSION"), "0x50014ee211e8fd40") == 0);

    /* no-WWN: clear the WWN-supported bit (word 87 bit 8) and zero words 108-111 */
    uint8_t nw[512];
    memcpy(nw, ata_sda_identify, sizeof nw);
    nw[2*87 + 1] &= ~0x01;                 /* clear bit 8 of word 87 (high byte bit 0) */
    for (int i = 108; i <= 111; i++) { nw[2*i] = 0; nw[2*i + 1] = 0; }
    struct uevent ev2;
    ata_id_decode(nw, 0, &ev2);
    assert(get(&ev2, "ID_WWN") == NULL);
    assert(get(&ev2, "ID_WWN_WITH_EXTENSION") == NULL);
    assert(strcmp(get(&ev2, "ID_MODEL"), "WDC_WD10EZEX-08WN4A0") == 0);  /* rest intact */

    /* feature-set decode (WD10EZEX: rotational SATA HDD, not ATAPI) */
    assert(strcmp(get(&ev, "ID_ATA_PERIPHERAL_DEVICE_TYPE"), "0") == 0);   /* ATA, not packet */
    assert(strcmp(get(&ev, "ID_ATA_ROTATION_RATE_RPM"), "7200") == 0);
    assert(strcmp(get(&ev, "ID_ATA_SATA"), "1") == 0);
    assert(strcmp(get(&ev, "ID_ATA_WRITE_CACHE"), "1") == 0);
    assert(get(&ev, "ID_ATA_FEATURE_SET_SMART") != NULL);
    assert(get(&ev, "ID_ATA_FEATURE_SET_SMART_ENABLED") != NULL);

    uint8_t zero[512] = {0};
    struct uevent ez;
    assert(ata_id_decode(zero, 0, &ez) == 0);
    assert(get(&ez, "ID_ATA") == NULL);

    uint8_t cd[512] = {0};
    cd[0] = 0xc0; cd[1] = 0x85;
    const char *sn = "QM00005             ", *fw = "2.5+    ",
               *mdl = "QEMU DVD-ROM                            ";
    for (int i = 0; i < 10; i++) { cd[2*(10+i)] = sn[2*i+1]; cd[2*(10+i)+1] = sn[2*i]; }
    for (int i = 0; i < 4; i++)  { cd[2*(23+i)] = fw[2*i+1]; cd[2*(23+i)+1] = fw[2*i]; }
    for (int i = 0; i < 20; i++) { cd[2*(27+i)] = mdl[2*i+1]; cd[2*(27+i)+1] = mdl[2*i]; }
    struct uevent ec;
    assert(ata_id_decode(cd, 1, &ec) > 0);
    assert(strcmp(get(&ec, "ID_TYPE"), "cd") == 0);
    assert(strcmp(get(&ec, "ID_BUS"), "ata") == 0);
    assert(strcmp(get(&ec, "ID_ATA_PERIPHERAL_DEVICE_TYPE"), "5") == 0);
    assert(strcmp(get(&ec, "ID_MODEL"), "QEMU_DVD-ROM") == 0);
    assert(strcmp(get(&ec, "ID_REVISION"), "2.5+") == 0);
    assert(strcmp(get(&ec, "ID_SERIAL"), "QEMU_DVD-ROM_QM00005") == 0);

    uint8_t cf[512];
    memcpy(cf, ata_sda_identify, sizeof cf);
    cf[0] = 0x8a; cf[1] = 0x84;
    struct uevent ef;
    ata_id_decode(cf, 0, &ef);
    assert(strcmp(get(&ef, "ID_TYPE"), "disk") == 0);

    printf("test_ata_id: OK\n");
    return 0;
}
