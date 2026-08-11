#include "../uaccess.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/acl.h>
#include <acl/libacl.h>

/* Does the file's access ACL contain user:uid with rw (read+write)? */
static int has_user_rw(const char *path, int uid) {
    acl_t acl = acl_get_file(path, ACL_TYPE_ACCESS);
    if (!acl) return -1;
    int found = 0;
    acl_entry_t ent;
    for (int r = acl_get_entry(acl, ACL_FIRST_ENTRY, &ent);
         r == 1; r = acl_get_entry(acl, ACL_NEXT_ENTRY, &ent)) {
        acl_tag_t tag;
        if (acl_get_tag_type(ent, &tag) != 0 || tag != ACL_USER) continue;
        uid_t *q = acl_get_qualifier(ent);
        if (!q) continue;
        if ((int)*q == uid) {
            acl_permset_t ps;
            if (acl_get_permset(ent, &ps) == 0
                && acl_get_perm(ps, ACL_READ) == 1
                && acl_get_perm(ps, ACL_WRITE) == 1)
                found = 1;
        }
        acl_free(q);
    }
    acl_free(acl);
    return found;
}

int main(void) {
    char tmpl[] = "/tmp/ua-apply-XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);

    /* No user:1000 entry to start. */
    assert(has_user_rw(tmpl, 1000) == 0);

    /* apply grants user:1000:rw */
    assert(ua_apply_node(tmpl, 1000) == 0);
    assert(has_user_rw(tmpl, 1000) == 1);

    /* idempotent: applying again is still fine, still granted */
    assert(ua_apply_node(tmpl, 1000) == 0);
    assert(has_user_rw(tmpl, 1000) == 1);

    /* clear removes the user:1000 entry */
    assert(ua_clear_node(tmpl, 1000) == 0);
    assert(has_user_rw(tmpl, 1000) == 0);

    /* clearing a node with no such entry is a no-op success */
    assert(ua_clear_node(tmpl, 1000) == 0);

    /* apply to a nonexistent node fails, not crashes */
    assert(ua_apply_node("/tmp/ua-apply-nonexistent-xyz", 1000) != 0);

    unlink(tmpl);
    printf("test_uaccess_apply: OK\n");
    return 0;
}
