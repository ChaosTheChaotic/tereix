#ifndef UTIL_H
#define UTIL_H

#include "arena.h"

const char *resolve_alloc(Arena *arena, const char *rel_path);
const char *load_file(const char *path);

#endif // !UTIL_H
