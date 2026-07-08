#include "string_builder.h"

void sb_init(StringBuilder *sb) {
  sb->cap = 2048;
  sb->len = 0;
  sb->buf = malloc(sb->cap);
  sb->buf[0] = '\0';
}

void sb_append_len(StringBuilder *sb, const char *str, size_t slen) {
  if (sb->len + slen + 1 > sb->cap) {
    sb->cap = (sb->len + slen + 1) * 2;
    sb->buf = realloc(sb->buf, sb->cap);
  }
  memcpy(sb->buf + sb->len, str, slen);
  sb->len += slen;
  sb->buf[sb->len] = '\0';
}

void sb_append(StringBuilder *sb, const char *str) {
  sb_append_len(sb, str, strlen(str));
}

void sb_free(StringBuilder *sb) { free(sb->buf); }
