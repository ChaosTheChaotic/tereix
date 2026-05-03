#ifndef TYPES_H
#define TYPES_H

#include "hashmap.h"
#include <stdbool.h>

#define SIZES(X) X(8) X(16) X(32) X(64) X(128) X(size)

#define AS_UNSIGNED(n) "u" #n,
#define AS_SIGNED(n) "i" #n,
#define AS_FLOAT(n) "f" #n,

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
  char *start;
  char *curr;
  unsigned int line;
  unsigned int col;

  HashMap kw_map;
  HashMap op_map;
  HashMap comp_map;
  HashMap type_kw_map;
} LexCtx;

struct AstNode;

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

typedef enum {
  AST_BINOP,
  AST_UOP,
  AST_ADDR_OF,
  AST_DEREF,
  AST_IDENTIF,
  AST_VAR_DECL,
  AST_NUM_LIT,
  AST_STR_LIT,
  AST_CHAR_LIT,
  AST_BOOL_LIT,
  AST_ARRAY_LIT,
  AST_IF,
  AST_BLOCK,
  AST_STRUCT,
  AST_UNION,
  AST_ENUM,
  AST_ENUM_MEMBER,
  AST_DEFER,
  AST_FOR,
  AST_WHILE,
  AST_FUNC,
  AST_FUNC_CALL,
  AST_PARAM,
  AST_RET,
  AST_INDEX,
  AST_MEMBER,
  AST_SWITCH,
  AST_CASE,
  AST_EXTERN,
  AST_USE,
  AST_NULL_LIT,
  AST_BREAK,
  AST_CONTINUE,
  AST_CAST,
  AST_PROGRAM, // The root node
} ASTN_TYPE;

struct Sym;

typedef struct AstNode {
  ASTN_TYPE type;
  struct AstNode *next;
  DataType eval_type;
  bool is_nested_sue;
  union {
    struct {
      Token val;
    } num_lit;
    struct {
      Token val;
    } str_lit;
    struct {
      Token val;
    } char_lit;
    struct {
      Token val;
    } bool_lit;
    struct {
      struct AstNode *elements;
    } array_lit;
    struct {
      Token val;
      struct Sym *res_sm;
    } identif;
    struct {
      Token op;
      struct AstNode *left;
      struct AstNode *right;
    } binop;
    struct {
      Token op;
      struct AstNode *operand;
      bool is_postfix;
    } unop;
    struct {
      Token id;
      DataType type;
      struct AstNode *init;
    } var_decl;
    struct {
      Token if_stmt;
      struct AstNode *check;
      struct AstNode *action;
      struct AstNode *elseAct; // else {} or else if () {}
    } if_check;
    struct {
      Token structn;
      struct AstNode *contents;
    } struct_def;
    struct {
      Token unionn;
      struct AstNode *contents;
    } union_def;
    struct {
      Token enumn;
      struct AstNode *contents;
    } enum_def;
    struct {
      Token name;
      struct AstNode *val;
    } enum_member;
    struct {
      Token defer;
      struct AstNode *contents;
    } defer_stmt;
    struct {
      Token for_stmt;
      struct AstNode *init;
      struct AstNode *check;
      struct AstNode *inc;
      struct AstNode *action;
    } for_loop;
    struct {
      Token while_stmt;
      struct AstNode *check;
      struct AstNode *action;
    } while_loop;
    struct {
      DataType type;
      Token id;
    } fn_param;
    struct {
      Token fn_name;
      DataType ret_type;
      bool is_async;
      bool is_inline;
      bool is_extern;
      struct AstNode *params;
      struct AstNode *block;
    } func_def;
    struct {
      Token ret_kw;
      struct AstNode *expr;
    } ret_stmt;
    struct {
      struct AstNode *first_stmt;
      bool is_async;
    } block;
    struct {
      struct AstNode *caller;
      struct AstNode *args;
    } func_call;
    struct {
      struct AstNode *base;
      struct AstNode *index;
    } index;
    struct {
      struct AstNode *base;
      Token name;
      Token type;
    } member;
    struct {
      struct AstNode *check;
      struct AstNode *cases;
      struct AstNode *default_case;
    } switch_stmt;
    struct {
      struct AstNode *val;
      struct AstNode *action;
    } case_stmt;
    struct {
      struct AstNode *contents;
    } extern_block;
    struct {
      Token path;
      Token alias; // .len == 0 if no alias
    } use_stmt;
    struct {
      Token val;
    } null_lit;
    struct {
      Token kw;
    } break_stmt;
    struct {
      Token kw;
    } continue_stmt;
    struct {
      DataType target;
      struct AstNode *op;
    } cast;
  } as;
} AstNode;

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
  Token op;
  bool is_unary;
  bool is_postfix;
  DataType cast_type;
} OpInfo;

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

static const char *kwlist[] = {
    SIZES(AS_UNSIGNED) SIZES(AS_SIGNED) SIZES(AS_FLOAT) "mut",
    "size",
    "bool",
    "str", // Technically should be parsed as char[] but oh well
    "void",
    "null",
    "char",
    "auto",
    "any",
    "static",
    "if",
    "for",
    "while",
    "ret",
    "defer",
    "else",
    "struct",
    "union",
    "enum",
    "async",
    "threadlocal",
    "inline",
    "switch",
    "case",
    "default",
    "extern",
    "use",
    "break",
    "continue",
};
static const size_t kwlistlen = sizeof(kwlist) / sizeof(kwlist[0]);

static const char *oplist[] = {"^",  "&",   "|",   "!",  "<<", ">>", "+",  "-",
                               "/",  "*",   "%",   "+=", "-=", "/=", "*=", "%=",
                               "^=", "<<=", ">>=", "++", "--", "&&", "||", "."};
static const size_t oplistlen = sizeof(oplist) / sizeof(oplist[0]);

static const char *complist[] = {"==", "!=", "<=", ">=", "<", ">"};
static const size_t complistlen = sizeof(complist) / sizeof(complist[0]);

static const char *typelist[] = {
    SIZES(AS_UNSIGNED) SIZES(AS_SIGNED) SIZES(AS_FLOAT) "mut",
    "size",
    "bool",
    "str", // Technically should be parsed as char[] but oh well
    "void",
    "null",
    "char",
    "auto",
    "any",
};
static const size_t typelistlen = sizeof(typelist) / sizeof(typelist[0]);

typedef struct {
  const char **paths;
  size_t count;
  size_t capacity;
} Worklist;

typedef struct {
  Arena *arena;
  HashMap mod_cache;

  const char
      *std_lib_env_path; // In case the user specifies a dir to go through for
                         // libraries in addition to the default one
} SemCtx;

typedef struct Module {
  const char *abs_path;
  const char *mod_name;
  AstNode *ast_root;

  HashMap local_symbols;
  HashMap imported_mods;
} Module;

typedef enum {
  SYM_FUNC,
  SYM_ENUM,
  SYM_STRUCT,
  SYM_UNION,
  SYM_VAR,
} SymKind;

typedef struct Sym {
  SymKind kind;
  Token name;
  AstNode *decl_node;
  bool is_imported_mod;
} Sym;

typedef struct {
  HashMap symbols; // Maps id strings to Sym*
} Scope;

typedef struct {
  Scope *scopes;
  size_t count;
  size_t capacity;
  Arena *arena;
} ScopeStack;

typedef enum { ACTION_VISIT_NODE, ACTION_POP_SCOPE } TravAction;

typedef struct {
  AstNode *node;
  TravAction action;
} TravItem;

typedef struct {
  AstNode *node;
  Token sue;
} FlattenFrame;

typedef struct {
  AstNode *node;
  int depth;
  const char *label;
} AstPrintItem;

typedef enum { TC_VISIT_CHILDREN, TC_EVAL_NODE } TCState;

typedef struct {
  AstNode *node;
  TCState state;
  DataType *expected;
  AstNode *curr_func;
} TCItem;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} StringBuilder;

typedef struct {
  AstNode *node;
  int step;
  AstNode *aux;
  AstNode *aux2;
  int flags;
} IterFrame;

#endif // !TYPES_H
