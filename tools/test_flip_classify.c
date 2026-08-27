/* Unit test for the flip eligibility classifier (link_is_critical).
 * Pure string logic — no live-system deps — so it runs anywhere.
 * Build: cc -o /tmp/tfc tools/test_flip_classify.c && /tmp/tfc */
#include "flip_classify.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void expect(const char *link, int want) {
    int got = link_is_critical(link);
    if (got != want) {
        printf("FAIL: link_is_critical(\"%s\") = %d, want %d\n", link, got, want);
        fails++;
    } else {
        printf("ok:   link_is_critical(\"%s\") = %d\n", link, got);
    }
}

int main(void) {
    /* fstab/boot-critical exact-path links: a MISSING one is harmful. */
    expect("disk/by-uuid/0f8b2c11-1111-2222-3333-444455556666", 1);
    expect("disk/by-partuuid/abcd-01", 1);
    expect("disk/by-label/root", 1);
    expect("disk/by-partlabel/EFI\\x20System", 1);

    /* eli's real SYM-MISS: a redundant by-path name for a keyboard already
     * reachable via a sibling link -> NOT critical -> harmless. */
    expect("input/by-path/platform-i8042-serio-0-event-kbd", 0);

    /* other non-critical convenience links -> harmless when missing. */
    expect("input/by-path/platform-INT33C3:00-hidraw", 0);
    expect("disk/by-id/ata-MATSHITA_DVD_UJ8E2", 0); /* by-id handled as known-debt elsewhere */
    expect("char/226:0", 0);
    expect("dri/by-path/pci-0000:00:02.0-card", 0);

    if (fails) { printf("\n%d test(s) FAILED\n", fails); return 1; }
    printf("\nall classifier tests passed\n");
    return 0;
}
