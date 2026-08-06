#include "../udev-parity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* udev_db_read_eprops: only E: lines captured, others ignored */
    char tmpl[] = "/tmp/parity-XXXXXX";
    char *dir = mkdtemp(tmpl); assert(dir);
    char p[256]; snprintf(p, sizeof p, "%s/c1:3", dir);
    FILE *f = fopen(p, "w");
    fputs("V:1\n", f);
    fputs("I:12345\n", f);
    fputs("E:ID_SERIAL=Foo_Bar_123\n", f);
    fputs("E:ID_INPUT=1\n", f);
    fputs("G:systemd\n", f);
    fputs("S:serial/by-id/xyz\n", f);
    fclose(f);

    struct uevent ev;
    assert(udev_db_read_eprops(p, &ev) == 0);
    assert(ev.n == 2);
    assert(strcmp(uevent_get(&ev, "ID_SERIAL"), "Foo_Bar_123") == 0);
    assert(strcmp(uevent_get(&ev, "ID_INPUT"), "1") == 0);
    assert(uevent_get(&ev, "V") == NULL && uevent_get(&ev, "G") == NULL);

    assert(udev_db_read_eprops("/no/such/file", &ev) == -1);

    /* parity_builtin_hint mapping (order-sensitive: _FROM_DATABASE wins) */
    assert(strcmp(parity_builtin_hint("ID_INPUT_KEYBOARD"), "input_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_NET_NAME_PATH"), "net_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_FS_UUID"), "blkid") == 0);
    assert(strcmp(parity_builtin_hint("ID_SERIAL"), "usb_id") == 0);
    assert(strcmp(parity_builtin_hint("ID_VENDOR_FROM_DATABASE"), "hwdb") == 0);
    assert(strcmp(parity_builtin_hint("ID_PATH"), "path_id") == 0);
    assert(strcmp(parity_builtin_hint("DEVNAME"), "") == 0);

    /* keycount add + sort */
    struct keycount tab[8]; int n = 0;
    keycount_add(tab, &n, 8, "ID_INPUT");
    keycount_add(tab, &n, 8, "ID_SERIAL");
    keycount_add(tab, &n, 8, "ID_INPUT");
    assert(n == 2);
    keycount_sort_desc(tab, n);
    assert(strcmp(tab[0].key, "ID_INPUT") == 0 && tab[0].count == 2);
    assert(strcmp(tab[1].key, "ID_SERIAL") == 0 && tab[1].count == 1);

    unlink(p); rmdir(dir);
    printf("test_parity: OK\n");
    return 0;
}
