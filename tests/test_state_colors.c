#include "../state_colors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(!strcmp(state_ansi(STATE_PERFECT), "\033[35m"));
    assert(!strcmp(state_ansi(STATE_FULL_TRUST), "\033[32m"));
    assert(!strcmp(state_ansi(STATE_FUNDAMENTAL), "\033[32m"));
    assert(!strcmp(state_ansi(STATE_SETTLED), "\033[32m"));
    assert(!strcmp(state_ansi(STATE_NEW_PROCESS), "\033[2;32m"));
    assert(!strcmp(state_ansi(STATE_RECOVERY), "\033[33m"));
    assert(!strcmp(state_ansi(STATE_FRICTION), "\033[33m"));
    assert(!strcmp(state_ansi(STATE_DORMANT), "\033[34m"));
    assert(!strcmp(state_ansi(STATE_EXCISED), "\033[31m"));
    assert(!strcmp(state_ansi(250), "\033[37m"));
    assert(state_from_name("FRICTION", 8) == STATE_FRICTION);
    assert(state_from_name("PERFECTX", 8) == -1);
    assert(state_from_name("PERFEC", 6) == -1);
    assert(!strcmp(state_ansi((uint8_t)state_from_name("EXCISED", 7)), "\033[31m"));
    assert(!strcmp(state_label(STATE_NEW_PROCESS), "NEW_PROCESS") && !strcmp(state_label(250), "?"));
    puts("test_state_colors: ok");
    return 0;
}
