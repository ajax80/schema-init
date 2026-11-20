#ifndef SCHEMA_VERIFY_GATE_H
#define SCHEMA_VERIFY_GATE_H

#include <stdio.h>

/* action: "reboot", "poweroff" or "reexec". Runs `schema-ctl verify --quiet` as a child
 * unless force, or a non-interactive reboot/poweroff. 0 proceed, 1 refuse (said why on out). */
int verify_gate(const char *action, int force, int interactive, FILE *out);

#endif
