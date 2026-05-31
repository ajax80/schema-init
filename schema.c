#include "schema.h"

static uint8_t popcount(uint32_t v) {
    uint8_t n = 0;
    while (v) { n += (uint8_t)(v & 1); v >>= 1; }
    return n;
}

void schema_instance_init(schema_instance_t *inst, uint32_t pid, uint8_t target) {
    inst->pid        = pid;
    inst->state      = STATE_NEW_PROCESS;
    inst->weight     = 0;
    inst->target_c   = target;
    inst->prev_state = STATE_SETTLED;
    inst->flags      = 0;
}

uint8_t schema_step(schema_instance_t *inst, uint32_t flags) {
    uint8_t w;
    inst->flags = flags;

    switch (inst->state) {
        case STATE_NEW_PROCESS:
            w = popcount(flags & F8_MASK);
            inst->weight     = w;
            inst->prev_state = STATE_NEW_PROCESS;
            inst->state      = (w >= SHIFT_THRESHOLD_8) ? STATE_FULL_TRUST : STATE_RECOVERY;
            break;

        case STATE_RECOVERY:
            w = popcount(flags & F9_MASK);
            inst->weight = w;
            inst->state  = (w >= SHIFT_THRESHOLD_9) ? STATE_SETTLED : STATE_FRICTION;
            break;

        case STATE_FRICTION:
            w = popcount(flags & F6_MASK);
            inst->weight = w;
            inst->state  = (w >= SHIFT_THRESHOLD_6) ? STATE_RECOVERY : STATE_EXCISED;
            break;

        case STATE_FULL_TRUST:
            if ((flags & F8_MASK) == F8_MASK)
                inst->state = STATE_FUNDAMENTAL;
            else if (!flags)
                inst->state = STATE_EXCISED;
            /* partial flags: stay in FULL_TRUST, re-evaluated next tick */
            break;
    }

    return inst->state;
}

const char *state_name(uint8_t s) {
    switch (s) {
        case STATE_FUNDAMENTAL: return "FUNDAMENTAL";
        case STATE_FRICTION:    return "FRICTION";
        case STATE_SETTLED:     return "SETTLED";
        case STATE_NEW_PROCESS: return "NEW_PROCESS";
        case STATE_RECOVERY:    return "RECOVERY";
        case STATE_FULL_TRUST:  return "FULL_TRUST";
        case STATE_PERFECT:     return "PERFECT";
        case STATE_EXCISED:     return "EXCISED";
        case STATE_DORMANT:     return "DORMANT";
        default:                return "UNKNOWN";
    }
}
