#define _GNU_SOURCE
#include "caps.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

static const struct { const char *name; int val; } cap_table[] = {
    { "CAP_CHOWN",            CAP_CHOWN },
    { "CAP_DAC_OVERRIDE",     CAP_DAC_OVERRIDE },
    { "CAP_DAC_READ_SEARCH",  CAP_DAC_READ_SEARCH },
    { "CAP_FOWNER",           CAP_FOWNER },
    { "CAP_FSETID",           CAP_FSETID },
    { "CAP_KILL",             CAP_KILL },
    { "CAP_SETGID",           CAP_SETGID },
    { "CAP_SETUID",           CAP_SETUID },
    { "CAP_SETPCAP",          CAP_SETPCAP },
    { "CAP_LINUX_IMMUTABLE",  CAP_LINUX_IMMUTABLE },
    { "CAP_NET_BIND_SERVICE", CAP_NET_BIND_SERVICE },
    { "CAP_NET_BROADCAST",    CAP_NET_BROADCAST },
    { "CAP_NET_ADMIN",        CAP_NET_ADMIN },
    { "CAP_NET_RAW",          CAP_NET_RAW },
    { "CAP_IPC_LOCK",         CAP_IPC_LOCK },
    { "CAP_IPC_OWNER",        CAP_IPC_OWNER },
    { "CAP_SYS_MODULE",       CAP_SYS_MODULE },
    { "CAP_SYS_RAWIO",        CAP_SYS_RAWIO },
    { "CAP_SYS_CHROOT",       CAP_SYS_CHROOT },
    { "CAP_SYS_PTRACE",       CAP_SYS_PTRACE },
    { "CAP_SYS_PACCT",        CAP_SYS_PACCT },
    { "CAP_SYS_ADMIN",        CAP_SYS_ADMIN },
    { "CAP_SYS_BOOT",         CAP_SYS_BOOT },
    { "CAP_SYS_NICE",         CAP_SYS_NICE },
    { "CAP_SYS_RESOURCE",     CAP_SYS_RESOURCE },
    { "CAP_SYS_TIME",         CAP_SYS_TIME },
    { "CAP_SYS_TTY_CONFIG",   CAP_SYS_TTY_CONFIG },
    { "CAP_MKNOD",            CAP_MKNOD },
    { "CAP_LEASE",            CAP_LEASE },
    { "CAP_AUDIT_WRITE",      CAP_AUDIT_WRITE },
    { "CAP_AUDIT_CONTROL",    CAP_AUDIT_CONTROL },
    { "CAP_SETFCAP",          CAP_SETFCAP },
    { "CAP_MAC_OVERRIDE",     CAP_MAC_OVERRIDE },
    { "CAP_MAC_ADMIN",        CAP_MAC_ADMIN },
    { "CAP_SYSLOG",           CAP_SYSLOG },
    { "CAP_WAKE_ALARM",       CAP_WAKE_ALARM },
    { "CAP_BLOCK_SUSPEND",    CAP_BLOCK_SUSPEND },
    { "CAP_AUDIT_READ",       CAP_AUDIT_READ },
#ifdef CAP_PERFMON
    { "CAP_PERFMON",          CAP_PERFMON },
#endif
#ifdef CAP_BPF
    { "CAP_BPF",              CAP_BPF },
#endif
#ifdef CAP_CHECKPOINT_RESTORE
    { "CAP_CHECKPOINT_RESTORE", CAP_CHECKPOINT_RESTORE },
#endif
};

int cap_name_to_val(const char *name) {
    for (size_t i = 0; i < sizeof(cap_table) / sizeof(cap_table[0]); i++)
        if (strcmp(cap_table[i].name, name) == 0)
            return cap_table[i].val;
    return -1;
}

int parse_cap_list(const char *csv, uint64_t *mask) {
    *mask = 0;
    char buf[512];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t l = strlen(tok);
        while (l && (tok[l - 1] == ' ' || tok[l - 1] == '\t')) tok[--l] = 0;
        if (!*tok) continue;
        int v = cap_name_to_val(tok);
        if (v < 0) return -1;
        *mask |= (uint64_t)1 << v;
    }
    return 0;
}

int apply_capabilities(uint64_t keep_mask) { (void)keep_mask; return 0; }  /* Task 2 */
int apply_no_new_privs(void) { return 0; }                                 /* Task 2 */
