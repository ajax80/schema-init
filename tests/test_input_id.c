#include "input_id.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_mask(void) {
    unsigned long a[IID_NWORDS];

    /* single word: word0 = rightmost */
    iid_parse_mask("8000", a);
    assert(iid_test_bit(a, 15));           /* 0x8000 = bit 15 */
    assert(!iid_test_bit(a, 14));

    /* multi-word: MSW first. "1f0000 0 0 0 0" -> word4 = 0x1f0000 */
    iid_parse_mask("1f0000 0 0 0 0", a);
    assert(iid_test_bit(a, 4*64 + 16));    /* BTN_LEFT = 0x110 = 272 */
    assert(iid_test_bit(a, 4*64 + 20));    /* 0x114 = 276 */
    assert(!iid_test_bit(a, 4*64 + 21));

    /* keyboard word0 mask */
    iid_parse_mask("fffffffffffffffe", a);
    assert((a[0] & 0xFFFFFFFEUL) == 0xFFFFFFFEUL);
    assert(!iid_test_bit(a, 0));
    assert(iid_test_bit(a, 1));

    /* short field: high words zero, no OOB */
    iid_parse_mask("0", a);
    assert(!iid_test_bit(a, 0));
    assert(!iid_test_bit(a, 700));

    /* NULL safe */
    iid_parse_mask(NULL, a);
    assert(!iid_test_bit(a, 0));

    /* any_bit range */
    iid_parse_mask("10000 0 0 0", a);      /* word3 bit16 = 3*64+16 = 208 */
    assert(iid_any_bit(a, 200, 220));
    assert(!iid_any_bit(a, 0, 200));

    printf("test_mask OK\n");
}

int main(void) {
    test_mask();
    printf("ALL input_id tests passed\n");
    return 0;
}
