#ifndef CAPS_H
#define CAPS_H

#include <stdint.h>
#include <sys/types.h>

/* CAP_* name -> integer value; -1 if unknown. */
int cap_name_to_val(const char *name);

/* Parse a comma list of CAP_* names into a keep-mask (bit N = CAP_N).
 * Returns 0 on success, -1 if any name is unknown. Empty input -> mask 0. */
int parse_cap_list(const char *csv, uint64_t *mask);

/* Restrict the process to exactly keep_mask: drop every other capability
 * from the bounding set and set permitted/effective/inheritable to keep_mask.
 * Runs while root, before setuid. Returns 0 on success, -1 on real failure. */
int apply_capabilities(uint64_t keep_mask);

/* Ambient caps for a service that drops to a non-root uid. Prep runs while
 * root, before initgroups/setgid/setuid; raise runs right after setuid. On
 * failure *step names the call that failed. */
int ambient_caps_prep(uint64_t mask, const char **step);
int ambient_caps_raise(uint64_t mask, const char **step);

/* prep + initgroups/setgid/setuid + raise, for a path with nothing between. */
int apply_ambient_caps(uint64_t mask, uid_t uid, gid_t gid, const char *user,
                       const char **step);

/* prctl(PR_SET_NO_NEW_PRIVS). Returns 0 on success, -1 on failure. */
int apply_no_new_privs(void);

#endif
