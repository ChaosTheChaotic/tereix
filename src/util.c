#include <limits.h>
#include <string.h>
#include "arena.h"

const char *resolve_alloc(Arena *arena, const char *rel_path) {
  char temp[PATH_MAX];
  if (realpath(rel_path, temp) == NULL)
    return NULL;
  size_t len = strlen(temp) + 1;
  char *perm_path = arena_alloc(arena, len);
  memcpy(perm_path, temp, len);
  return perm_path;
}
