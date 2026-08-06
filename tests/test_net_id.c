#include "net_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

static void nid_mkdirs(const char *p) {
    char t[PATH_MAX]; safe_copy(t, p, sizeof t);
    for (char *s = t + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, 0755);
}
static void nid_wf(const char *dir, const char *name, const char *val) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(val, f); fputc('\n', f); fclose(f);
}

static void test_gates(void) {
    char root[] = "/tmp/nidgateXXXXXX"; assert(mkdtemp(root));
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/eth0", root); nid_mkdirs(net);

    /* not stacked: ifindex == iflink */
    nid_wf(net, "ifindex", "3"); nid_wf(net, "iflink", "3");
    assert(nid_is_stacked(net) == 0);
    /* stacked: differ */
    nid_wf(net, "iflink", "2");
    assert(nid_is_stacked(net) == 1);

    /* arphrd */
    nid_wf(net, "type", "1");
    assert(nid_arphrd(net) == 1);

    /* prefix: plain ether -> en (uevent without DEVTYPE) */
    nid_wf(net, "uevent", "INTERFACE=eth0\nIFINDEX=3");
    char pfx[8];
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "en") == 0);

    /* wlan -> wl */
    nid_wf(net, "uevent", "INTERFACE=wlan0\nDEVTYPE=wlan");
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "wl") == 0);

    /* wwan -> ww */
    nid_wf(net, "uevent", "DEVTYPE=wwan");
    assert(nid_prefix(net, NID_ARPHRD_ETHER, pfx, sizeof pfx) == 0);
    assert(strcmp(pfx, "ww") == 0);

    /* slip -> sl, infiniband -> ib */
    assert(nid_prefix(net, NID_ARPHRD_SLIP, pfx, sizeof pfx) == 0 && strcmp(pfx, "sl") == 0);
    assert(nid_prefix(net, NID_ARPHRD_INFINIBAND, pfx, sizeof pfx) == 0 && strcmp(pfx, "ib") == 0);

    /* unsupported arphrd -> -1 */
    assert(nid_prefix(net, 772, pfx, sizeof pfx) != 0);   /* loopback */

    printf("test_gates OK\n");
}

int main(void) {
    test_gates();
    printf("ALL net_id tests passed\n");
    return 0;
}
