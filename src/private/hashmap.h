#ifndef HASHMAP_H
#define HASHMAP_H

#include "arena.h"
#include <string.h>

typedef struct HashEntry {
  const char *key;
  size_t key_len;
  void *value;
  struct HashEntry *next;
} HashEntry;

typedef struct {
  HashEntry **buckets;
  size_t capacity;
  size_t count;
  Arena *arena;
} HashMap;

void map_init(HashMap *map, Arena *arena, size_t capacity);

void map_resize(HashMap *map);

void map_free_buckets(HashMap *map);

void map_set(HashMap *map, const char *key, size_t key_len, void *value);

void *map_get(HashMap *map, const char *key, size_t key_len);

#endif // !HASHMAP_H
