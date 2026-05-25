#ifndef AST_TYPES_H
#define AST_TYPES_H

#include "arena.h"
#include "diag.h"
#include "types_core.h"

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
  AST_SIZEOF,
  AST_PROGRAM, // The root node
} ASTN_TYPE;

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
			Token use_kw;
			Token semicln;
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
    struct {
      bool is_type;
      DataType target_type;
      struct AstNode *target_expr;
    } sizeof_expr;
  } as;
} AstNode;

typedef struct {
  Token op;
  bool is_unary;
  bool is_postfix;
  DataType cast_type;
} OpInfo;

#define SIZES(X) X(8) X(16) X(32) X(64) X(128) X(size)

#define AS_UNSIGNED(n) "u" #n,
#define AS_SIGNED(n) "i" #n,
#define AS_FLOAT(n) "f" #n,

struct AstNode;

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
    SIZES(AS_UNSIGNED) SIZES(AS_SIGNED) SIZES(AS_FLOAT)
    "size",
    "bool",
    "str", // Technically should be parsed as char[] but oh well
    "void",
    "char",
    "auto",
    "any",
};
static const size_t typelistlen = sizeof(typelist) / sizeof(typelist[0]);

typedef struct {
  AstNode *node;
  int depth;
  const char *label;
} AstPrintItem;

AstNode *new_node(Arena *arena, ASTN_TYPE type);
void write_ast(const char *path);

void append_stmt(AstNode **head, AstNode *new_stmt);

AstNode *file_to_ast(Arena *arena, const char *path, bool partial);
AstNode *str_to_ast(Arena *arena, const char *file, const char *fpath, DiagList *diag_list, bool partial);

#endif // !AST_TYPES_H
