#ifndef HASHUTILS_H
#define HASHUTILS_H

#include <stdint.h>
#include <stdlib.h>

uint32_t hash_string(const char *key, size_t len);

uint32_t combine_hash(uint32_t current_hash, uint32_t new_value);

#endif // !HASHUTILS_H
