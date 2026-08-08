#include "cdrom_id.h"
#include "fixtures/cdrom_media_wardriver.h"
#include "fixtures/cdrom_media_blank.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static const char *get(const struct uevent *e, const char *k) {
    const char *v = uevent_get(e, k); return v ? v : "";
}

static void test_media_type(void) {
    struct uevent e;
    cdrom_media_type(cdrom_getconf_wardriver, sizeof cdrom_getconf_wardriver, (e.n=0, &e));
    assert(strcmp(get(&e, "ID_CDROM_MEDIA"), "1") == 0);
    assert(strcmp(get(&e, "ID_CDROM_MEDIA_DVD_R"), "1") == 0);

    cdrom_media_type(cdrom_getconf_blank, sizeof cdrom_getconf_blank, (e.n=0, &e));
    assert(strcmp(get(&e, "ID_CDROM_MEDIA"), "1") == 0);
    assert(strcmp(get(&e, "ID_CDROM_MEDIA_DVD_R"), "1") == 0);
    printf("test_cdrom_media media_type: OK\n");
}

static void test_discinfo(void) {
    struct uevent e;
    cdrom_discinfo_decode(cdrom_discinfo_wardriver, sizeof cdrom_discinfo_wardriver, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_STATE"),"appendable")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_COUNT"),"2")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT"),"2")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_NEXT"),"")==0);   /* deferred */

    cdrom_discinfo_decode(cdrom_discinfo_blank, sizeof cdrom_discinfo_blank, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_STATE"),"blank")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_SESSION_COUNT"),"1")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT"),"1")==0);
    printf("test_cdrom_media discinfo: OK\n");
}

int main(void) {
    test_media_type();
    test_discinfo();
    return 0;
}
