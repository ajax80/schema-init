#include "../uaccess.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void put(struct uevent *e, const char *k, const char *v) {
    safe_copy(e->key[e->n], k, UE_KEY_MAX);
    safe_copy(e->val[e->n], v, UE_VAL_MAX);
    e->n++;
}

int main(void) {
    /* ---- eligibility ---- */
    struct uevent e;
    e.n = 0; put(&e, "SUBSYSTEM", "sound");        assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "video4linux");  assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "media");        assert(uaccess_eligible(&e) == 1);
    e.n = 0; put(&e, "SUBSYSTEM", "dri");          assert(uaccess_eligible(&e) == 0);
    e.n = 0; put(&e, "SUBSYSTEM", "hidraw");       assert(uaccess_eligible(&e) == 0);
    e.n = 0; put(&e, "SUBSYSTEM", "block");        assert(uaccess_eligible(&e) == 0);
    e.n = 0;                                        assert(uaccess_eligible(&e) == 0);
    printf("test_uaccess eligible: OK\n");

    /* ---- active_uid ---- */
    char st[] = "/tmp/ua-seat-XXXXXX"; int sfd = mkstemp(st); assert(sfd >= 0);
    dprintf(sfd, "# This is private data. Do not parse.\nACTIVE=31\nACTIVE_UID=1000\nUIDS=1000\n");
    close(sfd);
    assert(uaccess_active_uid(st) == 1000);

    char st2[] = "/tmp/ua-seat2-XXXXXX"; int sfd2 = mkstemp(st2); assert(sfd2 >= 0);
    dprintf(sfd2, "ACTIVE=31\nUIDS=1000\n");   /* no ACTIVE_UID line */
    close(sfd2);
    assert(uaccess_active_uid(st2) == -1);
    assert(uaccess_active_uid("/tmp/ua-nonexistent-file-xyz") == -1);
    printf("test_uaccess active_uid: OK\n");

    /* ---- record ---- */
    char dirt[] = "/tmp/ua-dir-XXXXXX"; char *dir = mkdtemp(dirt); assert(dir);
    struct uevent s; s.n = 0;
    put(&s, "SUBSYSTEM", "sound"); put(&s, "DEVNAME", "snd/controlC0");
    put(&s, "MAJOR", "116"); put(&s, "MINOR", "7");
    assert(uaccess_record(dir, st, &s) == 0);

    char rec[512]; snprintf(rec, sizeof rec, "%s/c116:7", dir);
    FILE *f = fopen(rec, "r"); assert(f);
    char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    assert(strstr(buf, "DEVNODE=/dev/snd/controlC0"));
    assert(strstr(buf, "GRANT_UID=1000"));
    assert(strstr(buf, "ACL=user:1000:rw"));
    printf("test_uaccess record: OK\n");

    /* ---- ineligible clears a stale record at the same key ---- */
    struct uevent ie; ie.n = 0;
    put(&ie, "SUBSYSTEM", "dri"); put(&ie, "DEVNAME", "dri/card1");
    put(&ie, "MAJOR", "116"); put(&ie, "MINOR", "7");
    assert(uaccess_record(dir, st, &ie) == 0);   /* not eligible -> clears c116:7 */
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess ineligible-clears: OK\n");

    /* ---- no active uid -> no record ---- */
    assert(uaccess_record(dir, st2, &s) == 0);   /* st2 has no ACTIVE_UID */
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess no-uid-no-record: OK\n");

    /* ---- explicit clear is idempotent ---- */
    assert(uaccess_record(dir, st, &s) == 0);    /* recreate */
    assert(access(rec, F_OK) == 0);
    assert(uaccess_clear(dir, &s) == 0);
    assert(access(rec, F_OK) != 0);
    assert(uaccess_clear(dir, &s) == 0);         /* second clear: no-op */
    printf("test_uaccess clear-idempotent: OK\n");

    /* ---- wipe removes all records ---- */
    assert(uaccess_record(dir, st, &s) == 0);
    uaccess_wipe(dir);
    assert(access(rec, F_OK) != 0);
    printf("test_uaccess wipe: OK\n");

    printf("test_uaccess: ALL OK\n");
    return 0;
}
