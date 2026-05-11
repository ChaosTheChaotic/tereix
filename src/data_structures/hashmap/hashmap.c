#include "hashmap.h"

uint32_t hash_string(const char *key, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 16777619;
  }
  return hash;
}

void map_init(HashMap *map, Arena *arena, size_t capacity) {
  map->arena = arena;
  map->capacity = capacity;
  map->count = 0;
  map->buckets = calloc(capacity, sizeof(HashEntry *));
}

void map_resize(HashMap *map) {
  size_t new_capacity = map->capacity * 2;
  HashEntry **new_buckets = calloc(new_capacity, sizeof(HashEntry *));

  // Rehash existing entries
  for (size_t i = 0; i < map->capacity; i++) {
    HashEntry *entry = map->buckets[i];
    while (entry) {
      HashEntry *next = entry->next;
      uint32_t hash = hash_string(entry->key, entry->key_len);
      size_t index = hash % new_capacity;

      entry->next = new_buckets[index];
      new_buckets[index] = entry;

      entry = next;
    }
  }

  // Free the old buckets array so no memory is leaked
  free(map->buckets);

  map->buckets = new_buckets;
  map->capacity = new_capacity;
}

void map_free_buckets(HashMap *map) {
  if (map->buckets) {
    free(map->buckets);
    map->buckets = NULL;
  }
}

void map_set(HashMap *map, const char *key, size_t key_len, void *value) {
  // Resize if map exceeds 75% of capacity
  if (map->count >= (map->capacity * 3) / 4) {
    map_resize(map);
  }
  uint32_t hash = hash_string(key, key_len);
  size_t index = hash % map->capacity;

  HashEntry *entry = map->buckets[index];
  while (entry) {
    if (entry->key_len == key_len && strncmp(entry->key, key, key_len) == 0) {
      entry->value = value;
      return;
    }
    entry = entry->next;
  }

  HashEntry *new_entry = arena_alloc(map->arena, sizeof(HashEntry));
  new_entry->key = key;
  new_entry->key_len = key_len;
  new_entry->value = value;

  new_entry->next = map->buckets[index];
  map->buckets[index] = new_entry;
  map->count++;
}

void *map_get(HashMap *map, const char *key, size_t key_len) {
  uint32_t hash = hash_string(key, key_len);
  size_t index = hash % map->capacity;

  HashEntry *entry = map->buckets[index];
  while (entry) {
    if (entry->key_len == key_len && strncmp(entry->key, key, key_len) == 0) {
      return entry->value;
    }
    entry = entry->next;
  }
  return NULL; // Not found
}
