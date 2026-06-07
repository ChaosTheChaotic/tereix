#ifndef C_GEN_TYPES_H
#define C_GEN_TYPES_H

#include "ast_types.h"
#include "hashmap.h"
#include "sem_types.h"

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

typedef struct {
  AstNode *src;
  AstNode *dst;
} ClonePair;

typedef struct {
  AstNode *node;
  int step;
  size_t func_base;
  size_t loop_base;
  size_t block_base;
} LowerFrame;

HashMap *build_func_map(Arena *arena, AstNode *root);
bool output_to_c_and_compile(SemCtx *sem, const char *out_binary_name,
                             const char *compiler, const char **flags,
                             int flag_count, Arena *arena, Module *main_mod);
void mangle_mod_symbols(Arena *arena, Module *mod);

#endif // !C_GEN_TYPES_H
