#ifndef C_GEN_TYPES_H
#define C_GEN_TYPES_H

#include "ast_types.h"

typedef struct {
  AstNode *node;
  int step;
  AstNode *aux;
  AstNode *aux2;
  int flags;
} IterFrame;

typedef struct {
  AstNode *node;
  Token sue;
} FlattenFrame;

#endif // !C_GEN_TYPES_H
