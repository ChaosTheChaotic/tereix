#ifndef FORMATTER_H
#define FORMATTER_H

#include <stdbool.h>
#include <stddef.h>

#include "cli.h"
#include "ast_types.h"

typedef enum {
  FMT_PASCAL_CASE, // Types
  FMT_CAMEL_CASE,  // Variables
  FMT_SNAKE_CASE   // Functions
} FormatStyle;

typedef struct {
  AstNode *node;
  unsigned int depth;
  int step;
  AstNode *aux;
} FmtStackItem;

bool fmt_project(const CompileOptions *restrict opts);

#endif // !FORMATTER_H
