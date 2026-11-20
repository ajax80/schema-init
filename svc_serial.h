#ifndef SVC_SERIAL_H
#define SVC_SERIAL_H

#include <stdio.h>
#include "service.h"

/* One line per property: <name>.<key>=<value>, then for the hardening knobs a
 * TAB and src=explicit|default|dropped. Values escape '\\', newline and tab as
 * \\ \n \t; a list value is its items joined by one space, a space inside an
 * item written \s. Every property is always emitted, so unset → set is a value
 * change. Both return the number of lines written, sorted bytewise. */
int svc_serialize(const service_t *s, FILE *out);
int svc_serialize_all(const service_t *tab, int n, FILE *out);
/* every key svc_serialize writes, sorted, read off the emitters: the test
 * holds svc_keys.c's table to it */
int svc_serial_keys(const char *const **out);

/* shared with schema-ctl status <svc> */
const char *svc_hard_source(const service_t *s, uint8_t bit);
const char *svc_sc_deny_label(const service_t *s);
const char *svc_protect_system_name(uint8_t v);

#endif
