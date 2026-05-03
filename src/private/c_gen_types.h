#ifndef C_GEN_TYPES_H
#define C_GEN_TYPES_H

#include "ast_types.h"
#include "hashmap.h"

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

HashMap *build_func_map(Arena *arena, AstNode *root);
bool output_to_c_and_compile(AstNode *root, const char *out_binary_name, const char **flags, int flag_count, Arena *arena);

#endif // !C_GEN_TYPES_H
