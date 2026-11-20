#include "../verify_disk.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char devdir[PATH_MAX / 2];

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void mkd(const char *sub) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", devdir, sub);
    mkdir(p, 0755);
}

static fsclass_t cls(const char *src, const char *type, const char *opts) {
    struct mntent m = { (char *)src, "/x", (char *)type, (char *)opts, 0, 0 };
    return fstab_class(&m);
}

static int has(finding_t *f, int n, fkind_t k, const char *needle) {
    for (int i = 0; i < n; i++)
        if (f[i].kind == k && strstr(f[i].text, needle)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *tmpd = argc > 1 ? argv[1] : "/tmp";
    char p[PATH_MAX];
    snprintf(devdir, sizeof devdir, "%s/vk-dev", tmpd);
    mkdir(devdir, 0755);
    mkd("disk");
    mkd("disk/by-uuid");
    snprintf(p, sizeof p, "%s/sda1", devdir);
    put(p, "");
    snprintf(p, sizeof p, "%s/disk/by-uuid/PRESENT", devdir);
    assert(symlink("../../sda1", p) == 0);

    assert(cls("UUID=A", "btrfs", "defaults") == FS_LOCAL_REQ);
    assert(cls("/dev/sdb1", "ext4", "defaults") == FS_LOCAL_REQ);
    assert(cls("UUID=A", "ext4", "defaults,nofail") == FS_LOCAL_NOFAIL);
    assert(cls("UUID=A", "ext4", "nofail,x-systemd.device-timeout=5") == FS_LOCAL_NOFAIL);
    assert(cls("UUID=A", "ext4", "nofail,x-systemd.automount") == FS_LOCAL_NOFAIL);
    assert(cls("UUID=A", "ext4", "x-systemd.automount") == FS_AUTOMOUNT);
    assert(cls("UUID=A", "ext4", "x-systemd.device-timeout=5") == FS_LOCAL_REQ);
    assert(cls("UUID=A", "ext4", "noauto") == FS_NOAUTO);
    assert(cls("UUID=A", "ext4", "noauto,nofail") == FS_NOAUTO);
    assert(cls("UUID=A", "swap", "defaults") == FS_SWAP);
    assert(cls("/dev/zram0", "swap", "defaults") == FS_SWAP);
    assert(cls("srv:/exp", "nfs4", "defaults") == FS_NETWORK);
    assert(cls("//h/s", "cifs", "defaults") == FS_NETWORK);
    assert(cls("u@h:/", "fuse.sshfs", "defaults") == FS_LAYERED);
    assert(cls("/a:/b", "fuse.mergerfs", "defaults,nofail") == FS_LAYERED);
    assert(cls("tmpfs", "tmpfs", "defaults") == FS_VIRTUAL);
    assert(cls("none", "proc", "defaults") == FS_VIRTUAL);
    assert(cls("/dev/sda1", "tmpfs", "defaults") == FS_VIRTUAL);
    assert(cls("/srv/data", "none", "bind") == FS_BIND);
    assert(cls("/srv/data", "none", "rbind,ro") == FS_BIND);
    assert(cls("/dev/mapper/crypt", "ext4", "defaults") == FS_LAYERED);

    snprintf(p, sizeof p, "%s/fstab", devdir);
    put(p,
        "# comment\n"
        "UUID=PRESENT / btrfs subvol=root 0 0\n"
        "UUID=GONE /home btrfs subvol=home 0 0\n"
        "UUID=GONE2 /backup ext4 defaults,nofail 0 2\n"
        "UUID=GONE3 none swap defaults 0 0\n"
        "UUID=PRESENT none swap defaults 0 0\n"
        "srv:/exp /mnt/nas nfs defaults 0 0\n"
        "tmpfs /tmp tmpfs defaults 0 0\n"
        "/srv/data /mnt/bind none bind 0 0\n"
        "UUID=GONE4 /mnt/auto ext4 x-systemd.automount 0 0\n"
        "/a:/b /mnt/ocean fuse.mergerfs defaults,nofail 0 0\n"
        "UUID=GONE5 /mnt/off ext4 noauto 0 0\n");

    finding_t f[64];
    int n = disk_findings(p, NULL, devdir, f, 64);
    assert(n == 9);
    assert(has(f, n, F_FAIL, "/home") && has(f, n, F_FAIL, "UUID=GONE"));
    assert(has(f, n, F_WARN, "/backup") && has(f, n, F_WARN, "nofail"));
    assert(has(f, n, F_WARN, "UUID=GONE3") && has(f, n, F_WARN, "swap"));
    assert(has(f, n, F_NOTE, "/mnt/nas") && has(f, n, F_NOTE, "/tmp"));
    assert(has(f, n, F_NOTE, "/mnt/bind") && has(f, n, F_NOTE, "/mnt/auto"));
    assert(has(f, n, F_NOTE, "/mnt/ocean") && has(f, n, F_NOTE, "/mnt/off"));
    int fails = 0;
    for (int i = 0; i < n; i++) fails += f[i].kind == F_FAIL;
    assert(fails == 1);

    const char *km =
        "<3>[  10.1] blk_update_request: I/O error, dev sde, sector 100 op 0x0:(READ)\n"
        "<3>[  10.2] Buffer I/O error on dev sde1, logical block 5, async page read\n"
        "<3>[  10.3] EXT4-fs error (device sde1): ext4_find_entry:1455: inode #2\n"
        "<3>[  11.0] BTRFS error (device sdb1): bdev /dev/sdb1 errs: wr 0, rd 0, flush 0, corrupt 2, gen 0\n"
        "<4>[  12.0] BTRFS warning (device nvme0n1p3): csum failed, corrupt leaf\n"
        "<6>[  13.0] EXT4-fs (sda1): mounted filesystem\n"
        "<6>[  14.0] BTRFS info (device sda2): using crc32c\n";
    n = disk_findings("/nonexistent", km, devdir, f, 64);
    assert(n == 3);
    assert(has(f, n, F_WARN, "sde \xe2\x80\x94 3 I/O or filesystem errors in the kernel log since boot"));
    assert(has(f, n, F_WARN, "sdb \xe2\x80\x94 1 I/O or filesystem error"));
    assert(has(f, n, F_WARN, "nvme0n1 \xe2\x80\x94 1 "));
    assert(!strstr(f[0].text, "sda"));

    n = disk_findings("/nonexistent", "<3>blk_update_request: I/O error, dev sdc, sector 1\n<3>x I/O error, dev sdc, sector 2\n", devdir, f, 64);
    assert(n == 1 && has(f, n, F_WARN, "sdc \xe2\x80\x94 2 I/O errors in the kernel log since boot"));
    /* blkid-style quotes: UUID="…" names the same device (getmntent leaves them in) */
    snprintf(p, sizeof p, "%s/fstab-q", devdir);
    put(p, "UUID=\"PRESENT\" / btrfs subvol=root 0 0\n\"UUID=PRESENT\" /a ext4 defaults 0 0\n"
           "UUID=\"GONE\" /home btrfs defaults 0 0\n");
    n = disk_findings(p, NULL, devdir, f, 64);
    assert(n == 1 && has(f, n, F_FAIL, "/home") && !has(f, n, F_FAIL, "PRESENT"));
    assert(cls("UUID=\"A\"", "ext4", "defaults") == FS_LOCAL_REQ);

    /* optical, loop and zram devices are no disk the boot waits on */
    n = disk_findings("/nonexistent",
                      "<3>blk_update_request: I/O error, dev sr0, sector 1\n"
                      "<3>blk_update_request: I/O error, dev loop3, sector 9\n"
                      "<3>blk_update_request: I/O error, dev zram0, sector 9\n"
                      "<3>blk_update_request: I/O error, dev sdc, sector 2\n", devdir, f, 64);
    assert(n == 1 && has(f, n, F_WARN, "sdc"));
    puts("ok");
    return 0;
}
