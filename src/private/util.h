#ifndef UTIL_H
#define UTIL_H

#include "arena.h"

const char *resolve_alloc(Arena *arena, const char *rel_path);
const char *load_file(const char *path);

char *absolute_from_uri(const char *uri);
char *uri_from_absolute(const char *absolute);

const char *extract_mod_name(Arena *arena, const char *abs_path);

#endif // !UTIL_H
