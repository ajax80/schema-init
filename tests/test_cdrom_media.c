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

static void test_toc(void) {
    struct uevent e;
    cdrom_toc_decode(cdrom_toc_wardriver, sizeof cdrom_toc_wardriver, (e.n=0,&e));
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT_DATA"),"1")==0);
    assert(strcmp(get(&e,"ID_CDROM_MEDIA_TRACK_COUNT_AUDIO"),"")==0);  /* 0 -> omitted */
    printf("test_cdrom_media toc: OK\n");
}

#include "fixtures/cdrom_media_udf.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

static void test_iso9660(void) {
    char path[] = "/tmp/optfsXXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(ftruncate(fd, 32768 + 2048) == 0);
    assert(pwrite(fd, iso_pvd_wardriver, sizeof iso_pvd_wardriver, 32768)
           == (ssize_t)sizeof iso_pvd_wardriver);
    close(fd);
    struct uevent e; e.n = 0;
    assert(optical_fs_probe(path, &e) == 0);
    assert(strcmp(get(&e,"ID_FS_TYPE"),"iso9660")==0);
    assert(strcmp(get(&e,"ID_FS_LABEL"),"Wardriver.2026.1080p.WEBRip.x264")==0);
    assert(strcmp(get(&e,"ID_FS_SYSTEM_ID"),"LINUX")==0);
    assert(strcmp(get(&e,"ID_FS_UUID"),"2026-05-09-01-34-23-00")==0);
    assert(strstr(get(&e,"ID_FS_APPLICATION_ID"),"K3B") != NULL);
    unlink(path);
    printf("test_cdrom_media iso9660: OK\n");
}

static void test_udf(void) {
    char path[] = "/tmp/optudfXXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(ftruncate(fd, 257ULL*2048) == 0);
    assert(pwrite(fd, udf_nsr_lba19,   2048, 19ULL*2048)  == 2048);
    assert(pwrite(fd, udf_avdp_lba256, 2048, 256ULL*2048) == 2048);
    assert(pwrite(fd, udf_pvd_lba32,   2048, 32ULL*2048)  == 2048);
    assert(pwrite(fd, udf_lvd_lba35,   2048, 35ULL*2048)  == 2048);
    close(fd);
    struct uevent e; e.n = 0;
    assert(optical_fs_probe(path, &e) == 0);
    assert(strcmp(get(&e,"ID_FS_TYPE"),"udf")==0);
    assert(strcmp(get(&e,"ID_FS_LABEL"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_LOGICAL_VOLUME_ID"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_VOLUME_ID"),"POWERT_TOUR_DVD")==0);
    assert(strcmp(get(&e,"ID_FS_VOLUME_SET_ID"),"3655822E")==0);
    assert(strcmp(get(&e,"ID_FS_UUID"),"3655822e00000000")==0);
    assert(strcmp(get(&e,"ID_FS_VERSION"),"1.02")==0);
    assert(strstr(get(&e,"ID_FS_APPLICATION_ID"),"Apple")!=NULL);
    unlink(path);
    printf("test_cdrom_media udf: OK\n");
}

int main(void) {
    test_media_type();
    test_discinfo();
    test_toc();
    test_iso9660();
    test_udf();
    return 0;
}
