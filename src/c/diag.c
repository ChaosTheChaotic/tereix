#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void diaglist_init(DiagList *list, size_t initial_cap) {
  list->items = malloc(initial_cap * sizeof(Diag));
  list->cap = initial_cap;
  list->count = 0;
}

void diaglist_free(DiagList *list) {
  for (size_t i = 0; i < list->count; i++) {
    free((void *)list->items[i].message);
  }
  free(list->items);
  list->items = NULL;
  list->cap = list->count = 0;
}

void diaglist_add(DiagList *list, DiagSeverity sev, const char *message,
                  const char *file, unsigned int line, unsigned int col,
                  unsigned int end_line, unsigned int end_col) {
  if (list->count >= list->cap) {
    size_t new_cap = (list->cap == 0) ? 32 : list->cap * 2;
    Diag *new_items = realloc(list->items, new_cap * sizeof(Diag));
    if (!new_items) {
      fprintf(stderr, "Fatal: Out of memory while recording diagnostic: %s\n",
              message);
      return;
    }
    list->items = new_items;
    list->cap = new_cap;
  }
  // Convert 1 based lexer columns to 0 based LSP offsets
  Diag d = {.severity = sev,
            .message = strdup(message),
            .file = file,
            .start_line = line - 1,
            .start_char = col - 1,
            .end_line = end_line - 1,
            .end_char = end_col - 1};
  list->items[list->count++] = d;
}
