#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stdlib.h>
#include <string.h>

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} StringBuilder;

void sb_init(StringBuilder *sb);

void sb_append_len(StringBuilder *sb, const char *str, size_t slen);

void sb_append(StringBuilder *sb, const char *str);

void sb_free();

#endif // !STRING_BUILDER_H
