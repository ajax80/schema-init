#ifndef SCHEMA_H
#define SCHEMA_H

#include <stdint.h>

/* States */
#define STATE_FUNDAMENTAL   1
#define STATE_FRICTION      6
#define STATE_SETTLED       7
#define STATE_NEW_PROCESS   8
#define STATE_RECOVERY      9
#define STATE_FULL_TRUST    10
#define STATE_PERFECT       88
#define STATE_EXCISED       76

/* Weight maxima */
#define STATE_8_MAX         8
#define STATE_9_MAX         12
#define STATE_6_MAX         6

/* Shift thresholds — Jonathan's calibration 2026-05-23 */
#define SHIFT_THRESHOLD_8   5
#define SHIFT_THRESHOLD_9   8
#define SHIFT_THRESHOLD_6   3

/* State 8 flags — I vector */
#define F8_HW_EXISTS        (1 << 0)   /* binary exists on disk         */
#define F8_HW_RESPONDS      (1 << 1)   /* binary is executable          */
#define F8_DEP_PRESENT      (1 << 2)   /* dependency service exists     */
#define F8_DEP_STATE        (1 << 3)   /* dependency is stable          */
#define F8_MEM_AVAIL        (1 << 4)   /* memory allocation available   */
#define F8_MEM_SAFE         (1 << 5)   /* allocation within threshold   */
#define F8_PERM_PRESENT     (1 << 6)   /* credentials present           */
#define F8_PERM_AUTH        (1 << 7)   /* credentials authorized        */
#define F8_MASK             0xFF

/* State 9 flags — J vector */
#define F9_RETRY_COUNT      (1 << 0)   /* restart count not exceeded    */
#define F9_RETRY_WIN        (1 << 1)   /* cooldown window elapsed       */
#define F9_FALL_EXISTS      (1 << 2)   /* fallback service defined      */
#define F9_FALL_HEALTH      (1 << 3)   /* fallback is running           */
#define F9_MEM_FREE         (1 << 4)   /* memory can be reclaimed       */
#define F9_MEM_SUFF         (1 << 5)   /* reclaimed memory sufficient   */
#define F9_ESC_PATH         (1 << 6)   /* escalation path exists        */
#define F9_ESC_AUTH         (1 << 7)   /* escalation authorized         */
#define F9_TIMEOUT_WIN      (1 << 8)   /* within restart window         */
#define F9_TIMEOUT_EXT      (1 << 9)   /* timeout extensible            */
#define F9_PARTIAL_LOAD     (1 << 10)  /* subset of service can load    */
#define F9_PARTIAL_MIN      (1 << 11)  /* subset meets minimum weight   */
#define F9_MASK             0xFFF

/* State 6 flags — K vector */
#define F6_ERR_PATH         (1 << 0)   /* error is recoverable          */
#define F6_ERR_RES          (1 << 1)   /* recovery resources available  */
#define F6_ROLL_STATE       (1 << 2)   /* previous stable state exists  */
#define F6_ROLL_SAFE        (1 << 3)   /* rollback is safe              */
#define F6_ESC_LIMIT        (1 << 4)   /* failure count within limit    */
#define F6_ESC_PATTERN      (1 << 5)   /* failure pattern non-repeating */
#define F6_MASK             0x3F

typedef struct {
    uint8_t  state;
    uint8_t  weight;
    uint8_t  target_c;
    uint8_t  prev_state;
    uint32_t pid;
    uint32_t flags;
} schema_instance_t;

void        schema_instance_init(schema_instance_t *inst, uint32_t pid, uint8_t target);
uint8_t     schema_step(schema_instance_t *inst, uint32_t flags);
const char *state_name(uint8_t s);

#endif
