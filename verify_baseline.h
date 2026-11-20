#ifndef SCHEMA_VERIFY_BASELINE_H
#define SCHEMA_VERIFY_BASELINE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define VERIFY_DIR_DEFAULT "/var/lib/schema-init/verify"
#define BASELINE_MAGIC "schema-verify-baseline v1\n"

typedef struct { uint64_t since_ms; char sig[65]; } settle_t;

/* DIR/NAME replaced atomically, 0600, with the checks baseline_write makes */
int verify_state_put(const char *dir, const char *name, const char *data, size_t len);
int baseline_write(const char *dir, const char *check_out, size_t len, time_t now);
int settle_step(settle_t *st, const char *status_kv, uint64_t now_ms, unsigned window_s);
/* the failed non-critical services in status_kv, space-separated, into out; its length */
size_t settle_failed(const char *status_kv, char *out, size_t sz);
uint64_t settle_give_up_ms(unsigned window_s);
int pid1_exe_is_schema_init(const char *exe);
/* 1 when status --kv's booted_image= differs from image (dev:ino of /proc/1/exe now), or
 * its booted_version= from "schema-init <v>" (a --version line): PID 1 was re-exec'd
 * into a binary that never booted. A line missing (an older PID 1): that check passes. */
int reexecd_since_boot(const char *status_kv, const char *version_out, const char *image);

#endif
