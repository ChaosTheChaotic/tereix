#ifndef UTIL_H
#define UTIL_H

#include "arena.h"
#include "string_builder.h"
#include <stdbool.h>

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#define THREAD_LOCAL __thread
#elif __STDC_VERSION__ >= 201112L
#define THREAD_LOCAL _Thread_local
#else
#error "Compiler or platform does not support thread-local storage natively."
#endif

const char *resolve_alloc(Arena *arena, const char *rel_path);
const char *load_file(const char *path);

char *absolute_from_uri(const char *uri);
char *uri_from_absolute(const char *absolute);

const char *extract_mod_name(Arena *arena, const char *abs_path);

bool check_exists(const char *path);

bool file_is_identical(const char *path, StringBuilder *code);

void ensure_cache_dir();

const char *load_file_into_arena(Arena *arena, const char *path);

const char *normalize_module_path(Arena *arena, const char *path);

const char *resolve_module_path(Arena *arena,
                                const char *importing_file_abs_path,
                                const char *import_path);

#endif // !UTIL_H
