#include "ptrmap.h"

void ptrmap_init(PtrMap *map, uint32_t cap) {
  map->cap = cap;
  map->count = 0;
  map->entries = calloc(cap, sizeof(PtrMapEntry));
}

void ptrmap_expand(PtrMap *map) {
  uint32_t old_cap = map->cap;
  PtrMapEntry *old_entries = map->entries;
  map->cap = old_cap * 2;
  map->entries = calloc(map->cap, sizeof(PtrMapEntry));
  map->count = 0;

  for (uint32_t i = 0; i < old_cap; i++) {
    if (old_entries[i].ptr) {
      uintptr_t ptr = (uintptr_t)old_entries[i].ptr;
      uint32_t h = (uint32_t)(ptr ^ (ptr >> 32));
      h = (h >> 16) ^ h;
      h = h * 0x45d9f3b;
      h = (h >> 16) ^ h;
      uint32_t idx = h % map->cap;

      uint32_t attempts = 0;
      while (map->entries[idx].ptr != NULL) {
        idx = (idx + 1) % map->cap;
        if (++attempts >= map->cap) {
          fprintf(stderr, "Fatal: ptrmap_expand: table full during rehash\n");
          abort(); // Should never happen so abort to prevent corruption
        }
      }
      map->entries[idx] = old_entries[i];
      map->count++;
    }
  }
  free(old_entries);
}

bool ptrmap_put(PtrMap *map, AstNode *ptr, uint32_t idx) {
  if (!ptr)
    return false;
  if (map->count * 2 >= map->cap)
    ptrmap_expand(map);
  uint32_t i = (((uintptr_t)ptr >> 3) * 2654435761u) % map->cap;
  while (map->entries[i].ptr != NULL) {
    if (map->entries[i].ptr == ptr)
      return false;
    i = (i + 1) % map->cap;
  }
  map->entries[i].ptr = ptr;
  map->entries[i].idx = idx;
  map->count++;
  return true;
}

uint32_t ptrmap_get(PtrMap *map, AstNode *ptr) {
  if (!ptr || map->cap == 0)
    return UINT32_MAX;
  uint32_t i = (((uintptr_t)ptr >> 3) * 2654435761u) % map->cap;
  while (map->entries[i].ptr != NULL) {
    if (map->entries[i].ptr == ptr)
      return map->entries[i].idx;
    i = (i + 1) % map->cap;
  }
  return UINT32_MAX;
}
