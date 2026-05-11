#ifndef PARSE_TYPES_H
#define PARSE_TYPES_H

#include "ast_types.h"
#include "lex_types.h"
#include "types_core.h"

typedef enum {
  STATE_GLOBAL, // Looking for funcs, structs, global vars
  STATE_IN_FUNC,
  STATE_FUNC_BODY_DONE,
  STATE_IN_EXPR,
  STATE_INDEX_DONE,
  STATE_IN_ARRAY_LIT,
  STATE_ARRAY_ELEMENT_DONE,
  STATE_EXPR_STMT_DONE,
  STATE_IN_STRUCT_DEF,
  STATE_IN_UNION_DEF,
  STATE_IN_ENUM_DEF,
  STATE_ENUM_MEMBER_DONE,
  STATE_IN_IF_EXPECT_COND,
  STATE_IF_COND_DONE,
  STATE_IF_BODY_DONE,
  STATE_ELSE_BODY_DONE,
  STATE_PARSE_BLOCK, // Reusable state to parse { ... }
  STATE_BLOCK_DONE,
  STATE_BLOCK_EXPR_DONE,
  STATE_WHILE_COND_DONE,
  STATE_WHILE_BODY_DONE,
  STATE_FOR_INC_DONE,
  STATE_FOR_INIT_DONE,
  STATE_FOR_COND_DONE,
  STATE_FOR_BODY_DONE,
  STATE_FOR_INIT_START,
  STATE_FOR_INIT_DECL_DONE,
  STATE_IN_FUNC_ARGS,
  STATE_ARG_DONE,
  STATE_DEFER_DONE,
  STATE_RET_DONE,
  STATE_VAR_INIT_DONE,
  STATE_IN_EXTERN_BLOCK,
  STATE_SWITCH_COND_DONE,
  STATE_PARSE_SWITCH_BODY,
  STATE_CASE_EXPR_DONE,
  STATE_CASE_BODY_DONE,
  STATE_SWITCH_DONE,
  STATE_IF_ELSE_DONE,
} ParseState;

typedef struct {
  LexCtx *lex;
  Token curr;
  Token prev;
  Arena *arena;

  ParseState *state_stack;
  size_t state_count;
  size_t state_cap;

  AstNode **node_stack;
  size_t node_count;
  size_t node_cap;

  OpInfo *op_stack;
  size_t op_count;
  size_t op_cap;

  bool expect_operand;
  unsigned long int ag_depth;

  unsigned int err_count;
  bool panic_mode;
} ParseCtx;

void push_node(ParseCtx *ctx, AstNode *node);
bool parse(ParseCtx *ctx);
bool is_kw(LexCtx *ctx, const char *start, unsigned int len);

bool is_op(LexCtx *ctx, const char *start, unsigned int len);

bool is_compare(LexCtx *ctx, const char *start, unsigned int len);

bool is_punc(char c);
Token next_token(LexCtx *ctx);
DataType parse_type(ParseCtx *ctx);
#endif // !PARSE_TYPES_H
