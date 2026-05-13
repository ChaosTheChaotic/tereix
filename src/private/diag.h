// diagnostics.h
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  DIAG_ERROR = 1,
  DIAG_WARNING = 2,
  DIAG_INFO = 3,
  DIAG_HINT = 4
} DiagSeverity;

typedef struct {
  DiagSeverity severity;
  const char *message;
  const char *file; // URI or path
  // LSP uses 0 indexed lines so store them in 0 indexed form
  unsigned int start_line;
  unsigned int start_char;
  unsigned int end_line;
  unsigned int end_char;
} Diag;

typedef struct {
  Diag *items;
  size_t count;
  size_t cap;
} DiagList;

void diaglist_init(DiagList *list, size_t initial_cap);
void diaglist_free(DiagList *list);
void diaglist_add(DiagList *list, DiagSeverity sev, const char *message,
                  const char *file,
                  unsigned int line, // 1 based input, converted internally
                  unsigned int col, unsigned int end_line,
                  unsigned int end_col);
#endif
