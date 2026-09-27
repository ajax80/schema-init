#include "../../sdbus_auth.h"
#include <assert.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    if (size > 1 << 20) return 0;
    sdbus_conn c; memset(&c, 0, sizeof c);
    c.uid = 1000;
    size_t off = 0;
    while (off < size) {
        size_t n = 1 + data[off] % 64;
        off++;
        if (n > size - off) n = size - off;
        if (!n) break;
        int r = sdbus_auth_feed(&c, data + off, (int)n);
        off += n;
        assert(c.in_len >= 0 && c.in_len <= c.in_cap || c.in_len == 0);
        if (r != 0) break;
    }
    sdbus_conn_free_fields(&c);
    return 0;
}
