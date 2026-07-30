#ifndef TYPES_CORE_H
#define TYPES_CORE_H

#include <stdbool.h>
#include <stdlib.h>

typedef enum {
  TOKEN_ASSIGN,
  TOKEN_OP,
  TOKEN_IDENTIF,
  TOKEN_NUM_LIT,
  TOKEN_STR_LIT,
  TOKEN_CHAR_LIT,
  TOKEN_BOOL_LIT,
  TOKEN_KW,
  TOKEN_PUNC,
  TOKEN_COMPARE,
  TOKEN_EOF,
  TOKEN_UNKNOWN,
} TOKEN_TYPE;

typedef struct {
  const char *start;
  unsigned int len;
  TOKEN_TYPE type;
  unsigned int line, col;
} Token;

typedef struct {
  bool is_static;
  bool is_mut;
  bool is_custom;
  bool is_async;
  bool is_threadlocal;
  bool is_inline;
  bool is_extern;
  bool is_self;
  long int ptr_depth; // 2 for **type etc
  Token name;
  unsigned int array_dimens;  // 0 for not an array 1 for [] etc.
  struct AstNode **dim_sizes; // Per dimensions [][][] etc.
} DataType;

#endif // !TYPES_CORE_H
