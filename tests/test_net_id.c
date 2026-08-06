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

static void test_mac(void) {
    char root[] = "/tmp/nidmacXXXXXX"; assert(mkdtemp(root));
    char net[PATH_MAX]; snprintf(net, sizeof net, "%s/net/eth0", root); nid_mkdirs(net);
    char name[64];

    /* permanent, 6-byte -> enx<hex, colons stripped> */
    nid_wf(net, "addr_assign_type", "0");
    nid_wf(net, "addr_len", "6");
    nid_wf(net, "address", "a8:a1:59:0b:e8:ef");
    assert(nid_mac_name(net, "en", NID_ARPHRD_ETHER, name, sizeof name) == 0);
    assert(strcmp(name, "enxa8a1590be8ef") == 0);

    /* random assign type (1) -> no name */
    nid_wf(net, "addr_assign_type", "1");
    assert(nid_mac_name(net, "en", NID_ARPHRD_ETHER, name, sizeof name) != 0);

    /* permanent but not 6 bytes -> no name */
    nid_wf(net, "addr_assign_type", "0");
    nid_wf(net, "addr_len", "20");
    assert(nid_mac_name(net, "ib", NID_ARPHRD_INFINIBAND, name, sizeof name) != 0);

    printf("test_mac OK\n");
}

static int nid_has_val(const struct uevent *ev, const char *k, const char *v) {
    const char *g = uevent_get(ev, k);
    return g && strcmp(g, v) == 0;
}
static int nid_absent(const struct uevent *ev, const char *k) {
    return uevent_get(ev, k) == NULL;
}

static void test_pci(void) {
    char root[] = "/tmp/nidpciXXXXXX"; assert(mkdtemp(root));

    /* /sys/devices/pci0000:00/0000:00:1c.0/0000:06:00.0/net/enp6s0 */
    char pci[PATH_MAX];
    if ((size_t)snprintf(pci, sizeof pci, "%s/devices/pci0000:00/0000:00:1c.0/0000:06:00.0", root) >= sizeof pci) assert(0);
    char net[PATH_MAX];
    if ((size_t)snprintf(net, sizeof net, "%s/net/enp6s0", pci) >= sizeof net) assert(0);
    nid_mkdirs(net);
    nid_wf(pci, "dev_port", "0");
    /* config: 64 bytes, header type (offset 0x0e) = 0x00 -> single function */
    { char cf[PATH_MAX];
      if ((size_t)snprintf(cf, sizeof cf, "%s/config", pci) >= sizeof cf) assert(0);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z, 1, 64, f); fclose(f); }
    /* subsystem symlinks so pi_subsystem resolves */
    { char link[PATH_MAX], tgt[PATH_MAX];
      if ((size_t)snprintf(tgt, sizeof tgt, "%s/bus/pci", root) >= sizeof tgt) assert(0);
      nid_mkdirs(tgt);
      if ((size_t)snprintf(link, sizeof link, "%s/subsystem", pci) >= sizeof link) assert(0);
      symlink(tgt, link); }

    char busdir[PATH_MAX], sub[64];
    assert(nid_find_bus_parent(root, "/devices/pci0000:00/0000:00:1c.0/0000:06:00.0/net/enp6s0",
                               busdir, sizeof busdir, sub, sizeof sub) == 0);
    assert(strcmp(sub, "pci") == 0);

    struct uevent e; e.n = 0;
    nid_names_pci(root, busdir, "en", &e);
    assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enp6s0"));   /* bus 6, slot 0, func 0 single-fn */
    assert(nid_absent(&e, "ID_NET_NAME_SLOT"));              /* no hotplug slot */
    assert(nid_absent(&e, "ID_NET_NAME_ONBOARD"));

    /* nonzero domain + func>0 + dev_port>0: 0001:1a:00.1, dev_port 2 -> enP1p26s0f1d2 */
    char pci2[PATH_MAX];
    if ((size_t)snprintf(pci2, sizeof pci2, "%s/devices/pci0001:1a/0001:1a:00.1", root) >= sizeof pci2) assert(0);
    char net2[PATH_MAX];
    if ((size_t)snprintf(net2, sizeof net2, "%s/net/x", pci2) >= sizeof net2) assert(0);
    nid_mkdirs(net2);
    nid_wf(pci2, "dev_port", "2");
    { char cf[PATH_MAX];
      if ((size_t)snprintf(cf, sizeof cf, "%s/config", pci2) >= sizeof cf) assert(0);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z, 1, 64, f); fclose(f); }
    struct uevent e2; e2.n = 0;
    nid_names_pci(root, pci2, "en", &e2);
    assert(nid_has_val(&e2, "ID_NET_NAME_PATH", "enP1p26s0f1d2"));

    /* onboard: acpi_index 3 + label -> ID_NET_NAME_ONBOARD=eno3, ID_NET_LABEL_ONBOARD verbatim */
    nid_wf(pci, "acpi_index", "3");
    nid_wf(pci, "label", "Onboard LAN");
    struct uevent e3; e3.n = 0;
    nid_names_pci(root, pci, "en", &e3);
    assert(nid_has_val(&e3, "ID_NET_NAME_ONBOARD", "eno3"));
    assert(nid_has_val(&e3, "ID_NET_LABEL_ONBOARD", "Onboard LAN"));

    /* hotplug slot: /sys/bus/pci/slots/5/address = 0000:06:00 -> ID_NET_NAME_SLOT=ens5 */
    { char sd[PATH_MAX]; snprintf(sd, sizeof sd, "%s/bus/pci/slots/5", root); nid_mkdirs(sd);
      nid_wf(sd, "address", "0000:06:00"); }
    struct uevent e4; e4.n = 0;
    nid_names_pci(root, pci, "en", &e4);
    assert(nid_has_val(&e4, "ID_NET_NAME_SLOT", "ens5"));

    printf("test_pci OK\n");
}

static void test_other_buses(void) {
    char root[] = "/tmp/nidbusXXXXXX"; assert(mkdtemp(root));

    /* USB specifier: "1-1.2:1.0" -> ports "1.2"->"1u2", config 1 (omitted), iface 0 (omitted) */
    char spec[64];
    { char usb[PATH_MAX];
      if ((size_t)snprintf(usb, sizeof usb, "%s/1-1.2:1.0", root) >= sizeof usb) assert(0);
      nid_mkdirs(usb);
      assert(nid_usb_specifier(usb, spec, sizeof spec) == 0);
      assert(strcmp(spec, "u1u2") == 0); }

    /* USB specifier with config 2 + iface 3: "2-1:2.3" -> "u1c2i3" */
    { char usb[PATH_MAX];
      if ((size_t)snprintf(usb, sizeof usb, "%s/2-1:2.3", root) >= sizeof usb) assert(0);
      nid_mkdirs(usb);
      assert(nid_usb_specifier(usb, spec, sizeof spec) == 0);
      assert(strcmp(spec, "u1c2i3") == 0); }

    /* platform: ACPI id "ETH0000:02" (vendor eth, model 0x0000, instance 2) -> enaeth0i2 */
    { char plat[PATH_MAX];
      if ((size_t)snprintf(plat, sizeof plat, "%s/devices/platform/ETH0000:02", root) >= sizeof plat) assert(0);
      nid_mkdirs(plat);
      struct uevent e; e.n = 0;
      nid_names_platform(plat, "en", &e);
      assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enaeth0i2")); }

    /* malformed platform id (colon misplaced) -> nothing */
    { char plat[PATH_MAX];
      if ((size_t)snprintf(plat, sizeof plat, "%s/devices/platform/BADID00000", root) >= sizeof plat) assert(0);
      nid_mkdirs(plat);
      struct uevent e; e.n = 0;
      nid_names_platform(plat, "en", &e);
      assert(e.n == 0); }

    /* devicetree output format literal */
    { char name[32]; snprintf(name, sizeof name, "%sd%u", "en", 0u);
      assert(strcmp(name, "end0") == 0); }

    printf("test_other_buses OK\n");
}

static void test_build(void) {
    char root[] = "/tmp/nidbuildXXXXXX"; assert(mkdtemp(root));

    /* full PCI ethernet, permanent MAC */
    char pci[PATH_MAX];
    if ((size_t)snprintf(pci, sizeof pci, "%s/devices/pci0000:00/0000:06:00.0", root) >= sizeof pci) assert(0);
    char net[PATH_MAX];
    if ((size_t)snprintf(net, sizeof net, "%s/net/enp6s0", pci) >= sizeof net) assert(0);
    nid_mkdirs(net);
    nid_wf(net, "ifindex", "2"); nid_wf(net, "iflink", "2");
    nid_wf(net, "type", "1");
    nid_wf(net, "addr_assign_type", "0"); nid_wf(net, "addr_len", "6");
    nid_wf(net, "address", "a8:a1:59:0b:e8:ef");
    nid_wf(net, "uevent", "DEVTYPE=");
    nid_wf(pci, "dev_port", "0");
    { char cf[PATH_MAX];
      if ((size_t)snprintf(cf, sizeof cf, "%s/config", pci) >= sizeof cf) assert(0);
      FILE *f = fopen(cf, "wb"); assert(f); unsigned char z[64] = {0}; fwrite(z,1,64,f); fclose(f); }
    { char tgt[PATH_MAX];
      if ((size_t)snprintf(tgt, sizeof tgt, "%s/bus/pci", root) >= sizeof tgt) assert(0);
      nid_mkdirs(tgt);
      char link[PATH_MAX];
      if ((size_t)snprintf(link, sizeof link, "%s/subsystem", pci) >= sizeof link) assert(0);
      symlink(tgt, link); }

    struct uevent e;
    assert(net_id_build(root, "/devices/pci0000:00/0000:06:00.0/net/enp6s0", &e) == 0);
    assert(nid_has_val(&e, "ID_NET_NAMING_SCHEME", "v259"));
    assert(nid_has_val(&e, "ID_NET_NAME_MAC", "enxa8a1590be8ef"));
    assert(nid_has_val(&e, "ID_NET_NAME_PATH", "enp6s0"));

    /* stacked: iflink != ifindex -> nothing */
    char v[PATH_MAX];
    if ((size_t)snprintf(v, sizeof v, "%s/devices/virt/net/veth0", root) >= sizeof v) assert(0);
    nid_mkdirs(v);
    nid_wf(v, "ifindex", "9"); nid_wf(v, "iflink", "8"); nid_wf(v, "type", "1");
    assert(net_id_build(root, "/devices/virt/net/veth0", &e) == 0);
    assert(e.n == 0);

    /* unsupported ARPHRD (loopback 772) -> nothing */
    char lo[PATH_MAX];
    if ((size_t)snprintf(lo, sizeof lo, "%s/devices/virt/net/lo", root) >= sizeof lo) assert(0);
    nid_mkdirs(lo);
    nid_wf(lo, "ifindex", "1"); nid_wf(lo, "iflink", "1"); nid_wf(lo, "type", "772");
    assert(net_id_build(root, "/devices/virt/net/lo", &e) == 0);
    assert(e.n == 0);

    /* standalone virtual ether (docker0): not stacked, ether, no bus parent -> scheme only */
    char dk[PATH_MAX];
    if ((size_t)snprintf(dk, sizeof dk, "%s/devices/virt/net/docker0", root) >= sizeof dk) assert(0);
    nid_mkdirs(dk);
    nid_wf(dk, "ifindex", "4"); nid_wf(dk, "iflink", "4"); nid_wf(dk, "type", "1");
    nid_wf(dk, "addr_assign_type", "3"); nid_wf(dk, "addr_len", "6");
    nid_wf(dk, "address", "02:42:aa:bb:cc:dd"); nid_wf(dk, "uevent", "DEVTYPE=");
    assert(net_id_build(root, "/devices/virt/net/docker0", &e) == 0);
    assert(nid_has_val(&e, "ID_NET_NAMING_SCHEME", "v259"));
    assert(nid_absent(&e, "ID_NET_NAME_PATH"));
    assert(nid_absent(&e, "ID_NET_NAME_MAC"));   /* addr_assign_type 3, not permanent */

    printf("test_build OK\n");
}

int main(void) {
    test_gates();
    test_mac();
    test_pci();
    test_other_buses();
    test_build();
    printf("ALL net_id tests passed\n");
    return 0;
}
