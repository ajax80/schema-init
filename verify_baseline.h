#ifndef SCHEMA_VERIFY_BASELINE_H
#define SCHEMA_VERIFY_BASELINE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define VERIFY_DIR_DEFAULT "/var/lib/schema-init/verify"
#define BASELINE_MAGIC "schema-verify-baseline v1\n"

typedef struct { uint64_t since_ms; char sig[65]; } settle_t;

int baseline_write(const char *dir, const char *check_out, size_t len, time_t now);
int settle_step(settle_t *st, const char *status_kv, uint64_t now_ms, unsigned window_s);

#endif
