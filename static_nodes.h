#ifndef STATIC_NODES_H
#define STATIC_NODES_H

/* kmod-static-nodes parity: create the /dev nodes modules.devname declares so
 * the first open() autoloads the module (tun, uinput, vhost-net, ...). Every
 * entry point takes a root prefix ("" in PID 1) so tests run in a tmpdir. */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

struct sn_node { char devname[128]; char type; unsigned maj, min; };

/* modules.devname is root-owned, but PID 1 still refuses a name that would
 * land outside /dev. */
static inline int sn_path_ok(const char *rel) {
    if (rel[0] == '/' || rel[0] == '\0') return 0;
    for (const char *p = rel; (p = strstr(p, "..")); p += 2)
        if ((p == rel || p[-1] == '/') && (p[2] == '/' || p[2] == '\0')) return 0;
    return 1;
}

/* "<module> <devname> <c|b><major>:<minor>"; 1 parsed, 0 skip. */
static inline int sn_parse_devname_line(const char *line, struct sn_node *out) {
    char extra;
    if (line[0] == '#') return 0;
    if (sscanf(line, "%*s %127s %c%u:%u %c", out->devname, &out->type,
               &out->maj, &out->min, &extra) != 4) return 0;
    if (out->type != 'c' && out->type != 'b') return 0;
    return sn_path_ok(out->devname);
}

static inline void sn_mkparents(char *path) {
    char *slash = strrchr(path, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0755);
        *p = '/';
    }
    mkdir(path, 0755);
    *slash = '/';
}

static inline int sn_create_nodes(const char *root, const char *devname_file,
                                  char *created, size_t created_sz) {
    created[0] = '\0';
    FILE *f = fopen(devname_file, "re");
    if (!f) return 0;
    int n = 0;
    size_t used = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        struct sn_node nd;
        if (!sn_parse_devname_line(line, &nd)) continue;
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/dev/%s", root, nd.devname) >= sizeof path)
            continue;
        sn_mkparents(path);
        mode_t m = (nd.type == 'c' ? S_IFCHR : S_IFBLK) | 0600;
        if (mknod(path, m, makedev(nd.maj, nd.min)) != 0) {
            if (errno != EEXIST)
                printf("[schema-init] static node %s: %s\n", path, strerror(errno));
            continue;
        }
        n++;
        int w = snprintf(created + used, created_sz - used, "%s%s", used ? " " : "", nd.devname);
        if (w > 0 && used + (size_t)w < created_sz) used += (size_t)w;
        else created[used] = '\0';
    }
    fclose(f);
    return n;
}

#endif
