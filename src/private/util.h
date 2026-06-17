#ifndef UTIL_H
#define UTIL_H

#include "arena.h"
#include "string_builder.h"
#include <stdbool.h>

const char *resolve_alloc(Arena *arena, const char *rel_path);
const char *load_file(const char *path);

char *absolute_from_uri(const char *uri);
char *uri_from_absolute(const char *absolute);

const char *extract_mod_name(Arena *arena, const char *abs_path);

bool check_exists(const char *path);

bool file_is_identical(const char *path, StringBuilder *code);

void ensure_cache_dir();

#endif // !UTIL_H
