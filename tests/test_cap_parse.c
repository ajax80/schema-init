#include "../caps.h"
#include <assert.h>
#include <stdio.h>
#include <linux/capability.h>

int main(void) {
    uint64_t m;

    assert(parse_cap_list("CAP_NET_BIND_SERVICE", &m) == 0);
    assert(m == ((uint64_t)1 << CAP_NET_BIND_SERVICE));

    assert(parse_cap_list("CAP_SYS_TIME,CAP_NET_BIND_SERVICE", &m) == 0);
    assert(m == (((uint64_t)1 << CAP_SYS_TIME) | ((uint64_t)1 << CAP_NET_BIND_SERVICE)));

    assert(parse_cap_list(" CAP_CHOWN , CAP_KILL ", &m) == 0);
    assert(m == (((uint64_t)1 << CAP_CHOWN) | ((uint64_t)1 << CAP_KILL)));

    assert(parse_cap_list("", &m) == 0 && m == 0);

    assert(parse_cap_list("CAP_BOGUS", &m) == -1);

    assert(cap_name_to_val("CAP_CHOWN") == CAP_CHOWN);
    assert(cap_name_to_val("not_a_cap") == -1);

    printf("all cap-parse tests passed\n");
    return 0;
}
