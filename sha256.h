#ifndef SCHEMA_SHA256_H
#define SCHEMA_SHA256_H

#include <stddef.h>

void sha256_hex(const void *data, size_t len, char out[65]);

#endif
