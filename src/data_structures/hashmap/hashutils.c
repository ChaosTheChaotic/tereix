#include "hashutils.h"

uint32_t hash_string(const char *key, size_t len) {
	if (!key) return 0;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619;
  }
  return hash;
}

uint32_t combine_hash(uint32_t current_hash, uint32_t new_value) {
  const uint32_t fnv_prime = 16777619u;

  current_hash = (current_hash ^ (new_value & 0xFF)) * fnv_prime;
  current_hash = (current_hash ^ ((new_value >> 8) & 0xFF)) * fnv_prime;
  current_hash = (current_hash ^ ((new_value >> 16) & 0xFF)) * fnv_prime;
  current_hash = (current_hash ^ ((new_value >> 24) & 0xFF)) * fnv_prime;

  return current_hash;
}
