#include "../pi_bootguard.h"
#include <assert.h>
#include <string.h>

static void decide_fresh(void) {
    struct pi_state st = { 0, PI_CURRENT };
    assert(pi_decide(&st, 1) == PI_CURRENT && st.attempts == 1);
    assert(pi_decide(&st, 1) == PI_CURRENT && st.attempts == 2);
}

static void decide_third_with_kg(void) {
    struct pi_state st = { 2, PI_CURRENT };
    assert(pi_decide(&st, 1) == PI_KNOWN_GOOD);
    assert(st.attempts == 3 && st.last_target == PI_KNOWN_GOOD);
}

static void decide_third_no_kg(void) {
    struct pi_state st = { 2, PI_CURRENT };
    assert(pi_decide(&st, 0) == PI_SYSTEMD);
    assert(st.attempts == 0 && st.last_target == PI_SYSTEMD);
}

static void decide_kg_failed(void) {
    struct pi_state st = { 3, PI_KNOWN_GOOD };
    assert(pi_decide(&st, 1) == PI_SYSTEMD);
    assert(st.attempts == 0 && st.last_target == PI_SYSTEMD);
}

static void decide_after_systemd(void) {
    struct pi_state st = { 3, PI_KNOWN_GOOD };
    assert(pi_decide(&st, 1) == PI_SYSTEMD);
    assert(pi_decide(&st, 1) == PI_CURRENT);
    assert(st.attempts == 1 && st.last_target == PI_CURRENT);
}

static void decide_garbage_state(void) {
    static const char *bad[] = {
        "", "attempts=", "\x01\xff", "attempts=99999999999",
        "attempts=1\n", "last_target=systemd\n",
        "attempts=-1\nlast_target=systemd\n",
        "attempts=2\nlast_target=bogus\n",
        "attempts=2x\nlast_target=systemd\n",
        "attempts=2\nlast_target=\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        struct pi_state st = { 7, PI_SYSTEMD };
        assert(pi_state_parse(bad[i], &st) == 0);
        assert(st.attempts == 0 && st.last_target == PI_CURRENT);
    }
    struct pi_state ok = { 0, PI_CURRENT };
    assert(pi_state_parse("attempts=2\nlast_target=known-good\n", &ok) == 0);
    assert(ok.attempts == 2 && ok.last_target == PI_KNOWN_GOOD);
}

static void roundtrip(void) {
    char buf[64];
    struct pi_state a = { 3, PI_KNOWN_GOOD }, b;
    int n = pi_state_format(&a, buf, sizeof buf);
    assert(n == (int)strlen(buf));
    assert(strcmp(buf, "attempts=3\nlast_target=known-good\n") == 0);
    assert(pi_state_parse(buf, &b) == 0);
    assert(a.attempts == b.attempts && a.last_target == b.last_target);
    volatile size_t small = 5;
    assert(pi_state_format(&a, buf, small) == -1);
}

int main(void) {
    decide_fresh();
    decide_third_with_kg();
    decide_third_no_kg();
    decide_kg_failed();
    decide_after_systemd();
    decide_garbage_state();
    roundtrip();
    return 0;
}
