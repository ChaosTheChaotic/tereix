#ifndef SEM_TYPES_H
#define SEM_TYPES_H

#include "arena.h"
#include "ast_serde.h"
#include "ast_types.h"
#include "diag.h"
#include "hashmap.h"

typedef struct {
  Arena *arena;
  HashMap mod_cache;
  DiagList *diags;

  const char
      *std_lib_env_path; // In case the user specifies a dir to go through for
                         // libraries in addition to the default one
#ifdef ENABLE_THREADS
  pthread_mutex_t mutex;
#endif
} SemCtx;

typedef struct Module {
  const char *abs_path;
  const char *mod_name;
  const char *content;
  AstNode *ast_root;

  Arena *mod_arena;

  uint64_t content_hash;
  uint64_t interface_hash;
  DeclMetadata *meta;
  bool is_dirty;
  bool interface_changed;
  bool needs_cache_write;

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
  const char *fpath;
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
  bool is_top_level;
} TCItem;

typedef enum { ACTION_VISIT_NODE, ACTION_POP_SCOPE } TravAction;

typedef struct {
  AstNode *node;
  TravAction action;
  bool is_top_level;
} TravItem;

void sem_init(SemCtx *ctx, Arena *arena);
void sem_deinit(SemCtx *ctx);

Module *new_mod(Arena *arena, const char *abs_path, const char *mod_name,
                AstNode *ast);

bool get_numeric_info(DataType t, int *width, bool *is_signed, bool *is_float);

bool resolve_imports(Arena *arena, SemCtx *sem);
bool collect_mod_symbols(Arena *arena, Module *mod, SemCtx *ctx);
void scope_stack_init(ScopeStack *ss, Arena *arena);
void resolve_scopes(Arena *arena, Module *mod, ScopeStack *ss, SemCtx *ctx);
void type_check_ast(Arena *arena, AstNode *root, SemCtx *ctx);
void run_storage_duration_pass(AstNode *root, DiagList *diags,
                               const char *fpath);

void propagate_dirty_state(SemCtx *ctx);

void sem_report(SemCtx *ctx, DiagSeverity sev, Token token, const char *fmt,
                ...);

#endif // !SEM_TYPES_U
