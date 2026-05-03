#ifndef SEM_TYPES_h
#define SEM_TYPES_h

#include "ast_types.h"
#include "arena.h"
#include "hashmap.h"

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

typedef enum { TC_VISIT_CHILDREN, TC_EVAL_NODE } TCState;

typedef struct {
  AstNode *node;
  TCState state;
  DataType *expected;
  AstNode *curr_func;
} TCItem;

typedef enum { ACTION_VISIT_NODE, ACTION_POP_SCOPE } TravAction;

typedef struct {
  AstNode *node;
  TravAction action;
} TravItem;

#endif // !SEM_TYPES_h
