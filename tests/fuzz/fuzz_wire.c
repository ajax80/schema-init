#include "../../sdbus_wire.h"
#include <assert.h>
#include <stdio.h>

static int same(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    if (size > 1 << 20) return 0;
    unsigned char *buf = malloc(size ? size : 1);
    memcpy(buf, data, size);
    sdbus_wire_msg m;
    int t = sdbus_wire_parse(buf, (int)size, &m);
    if (t > 0) {
        assert(t <= (int)size);
        unsigned char *out = NULL; int outlen = 0;
        assert(sdbus_wire_reforward(buf, &m, ":1.42", &out, &outlen) == 0);
        sdbus_wire_msg r;
        int t2 = sdbus_wire_parse(out, outlen, &r);
        if (t2 != outlen) { fprintf(stderr, "reforward output does not reparse: %d vs %d\n", t2, outlen); abort(); }
        assert(r.type == m.type && r.serial == m.serial && r.body_len == m.body_len);
        assert(same(r.sender, ":1.42"));
        assert(same(r.path, m.path) && same(r.interface, m.interface) && same(r.member, m.member));
        assert(same(r.error_name, m.error_name) && same(r.destination, m.destination));
        assert(same(r.signature, m.signature));
        assert(r.has_reply_serial == m.has_reply_serial && r.reply_serial == m.reply_serial);
        assert(r.has_unix_fds == m.has_unix_fds && r.unix_fds == m.unix_fds);
        assert(!memcmp(out + r.body_offset, buf + m.body_offset, m.body_len));
        free(out);
    }
    free(buf);
    return 0;
}
