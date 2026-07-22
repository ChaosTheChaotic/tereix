#include "ast_visitor.h"
#include "sem_types.h"
#include "util.h"
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_THREADS
#include <pthread.h>

pthread_mutex_t sem_global_lock;
#endif
THREAD_LOCAL Module *sem_current_mod = NULL;
Module *sem_main_mod = NULL; // Set once then read-only so should be safe

typedef struct {
  Module *mod;
  Module *parent_mod;
  AstNode *use_stmt;
} ImportRelation;

#define MAX_IMPORT_RELATIONS 1024
static ImportRelation import_relations[MAX_IMPORT_RELATIONS];
static int import_relations_count = 0;

void propagate_dirty_state(SemCtx *ctx) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < ctx->mod_cache.capacity; i++) {
      HashEntry *entry = ctx->mod_cache.buckets[i];
      while (entry) {
        Module *mod = (Module *)entry->value;
        if (!mod || mod == (Module *)1) {
          entry = entry->next;
          continue;
        }
        if (mod->is_dirty) {
          entry = entry->next;
          continue;
        }

        for (size_t j = 0; j < mod->imported_mods.capacity; j++) {
          HashEntry *imp_entry = mod->imported_mods.buckets[j];
          while (imp_entry) {
            Module *imported = (Module *)imp_entry->value;
            if (imported->is_dirty || imported->interface_changed) {
              mod->is_dirty = true;
              changed = true;
              break;
            }
            imp_entry = imp_entry->next;
          }
          if (mod->is_dirty)
            break;
        }
        entry = entry->next;
      }
    }
  }
}

void record_import(Module *mod, Module *parent_mod, AstNode *use_stmt) {
  for (int i = 0; i < import_relations_count; i++) {
    if (import_relations[i].mod == mod) {
      return;
    }
  }
  if (import_relations_count < MAX_IMPORT_RELATIONS) {
    import_relations[import_relations_count++] =
        (ImportRelation){mod, parent_mod, use_stmt};
  }
}

ImportRelation *get_import_relation(Module *mod) {
  for (int i = 0; i < import_relations_count; i++) {
    if (import_relations[i].mod == mod) {
      return &import_relations[i];
    }
  }
  return NULL;
}

void sem_report(SemCtx *ctx, DiagSeverity sev, Token token, const char *fmt,
                ...) {
  va_list args;
  va_start(args, fmt);

  char *msg = NULL;
  int len = vasprintf(&msg, fmt, args);
  va_end(args);

  if (len < 0)
    return;
  if (!msg)
    return;

  // Always use the true origin of the error
  const char *report_file = sem_current_mod ? sem_current_mod->abs_path : NULL;
  int report_line = token.line;
  int report_col = token.col;
  int report_len = (int)token.len;

  if (ctx && ctx->diags) {
    // Report the exact error location in the module it actually occurred in
#ifdef ENABLE_THREADS
    pthread_mutex_lock(&ctx->mutex);
#endif
    diaglist_add(ctx->diags, sev, msg, report_file, report_line, report_col,
                 report_line, report_col + report_len);
#ifdef ENABLE_THREADS
    pthread_mutex_unlock(&ctx->mutex);
#endif

    // Transitevely bubble the error up to the main modules use statement
    if (sem_current_mod != NULL && sem_current_mod != sem_main_mod) {
      Module *curr = sem_current_mod;
      while (curr && curr != sem_main_mod) {
        ImportRelation *rel = get_import_relation(curr);

        if (rel && rel->parent_mod && rel->use_stmt) {
          // Once reached the main module, attach the enhanced diagnostic
          if (rel->parent_mod == sem_main_mod) {
            char *enhanced_msg =
                malloc(strlen(msg) + strlen(sem_current_mod->mod_name) + 32);
            sprintf(enhanced_msg, "Error in module '%s': %s",
                    sem_current_mod->mod_name, msg);

            Token use_tok = rel->use_stmt->as.use_stmt.path;
            diaglist_add(ctx->diags, sev, enhanced_msg,
                         rel->parent_mod->abs_path, use_tok.line, use_tok.col,
                         use_tok.line, use_tok.col + use_tok.len);

            free(enhanced_msg);
            break;
          }
          curr = rel->parent_mod;
        } else {
          break;
        }
      }
    }
    free(msg);
  } else {
    // CLI Fallback
    if (report_file) {
      fprintf(stderr, "%s:%d:%d: %s\n", report_file, report_line, report_col,
              msg);
    } else {
      fprintf(stderr, "%s\n", msg);
    }
    free(msg);
  }
}

void sem_init(SemCtx *ctx, Arena *arena) {
  ctx->arena = arena;
  map_init(&ctx->mod_cache, arena, 1024);
  ctx->std_lib_env_path = getenv("TX_LIB_SEARCH_PATH");
#ifdef ENABLE_THREADS
  pthread_mutex_init(&ctx->mutex, NULL);
#endif

  import_relations_count = 0;
  sem_current_mod = NULL;
  sem_main_mod = NULL;
}

void sem_deinit(SemCtx *ctx) {
  // Free module maps
  for (size_t i = 0; i < ctx->mod_cache.capacity; i++) {
    HashEntry *entry = ctx->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      map_free_buckets(&mod->local_symbols);
      map_free_buckets(&mod->imported_mods);
      entry = entry->next;
    }
  }
#ifdef ENABLE_THREADS
  pthread_mutex_destroy(&ctx->mutex);
#endif
  map_free_buckets(&ctx->mod_cache);
}

Sym *new_sym(Arena *arena, SymKind kind, Token name, AstNode *decl,
             const char *fpath) {
  Sym *s = arena_alloc(arena, sizeof(Sym));
  s->kind = kind;
  s->name = name;
  s->decl_node = decl;
  s->is_imported_mod = false;
  s->fpath = fpath;
  return s;
}

Module *new_mod(Arena *arena, const char *abs_path, const char *mod_name,
                AstNode *ast) {
  Module *m = arena_alloc(arena, sizeof(Module));
  m->abs_path = abs_path;
  m->mod_name = mod_name;
  m->ast_root = ast;
  m->mod_arena = NULL;

  map_init(&m->local_symbols, arena, 128);
  map_init(&m->imported_mods, arena, 32);
  return m;
}

bool get_numeric_info(DataType t, int *width, bool *is_signed, bool *is_float) {
  if (t.ptr_depth > 0 || t.array_dimens > 0 || t.is_custom)
    return false;
  if (t.name.len < 2)
    return false;

  if (t.name.len == 4 && strncmp(t.name.start, "size", 4) == 0) {
    *is_signed = false;
    *is_float = false;
    *width = 64;
    return true;
  }

  if (t.name.len == 4 && strncmp(t.name.start, "char", 4) == 0) {
    *is_signed = false;
    *is_float = false;
    *width = 8;
    return true;
  }

  if (t.name.len == 5 && strncmp(t.name.start + 1, "size", 4) == 0) {
    char first = t.name.start[0];
    if (first == 'u') {
      *is_signed = false;
      *is_float = false;
    } else if (first == 'i') {
      *is_signed = true;
      *is_float = false;
    } else if (first == 'f') {
      *is_signed = true;
      *is_float = true;
    } else {
      return false;
    }
    *width = 64;
    return true;
  }

  char kind = t.name.start[0];
  if (kind == 'u' || kind == 'i' || kind == 'f') {
    *is_signed = (kind == 'i' || kind == 'f');
    *is_float = (kind == 'f');
    *width = atoi(t.name.start + 1);
    return true;
  }
  return false;
}

bool is_type_compatible(DataType target, DataType source, bool is_explicit) {
  if (target.name.len == 3 && strncmp(target.name.start, "any", 3) == 0) {
    return true;
  }
  if (source.name.len == 3 && strncmp(source.name.start, "any", 3) == 0) {
    return true;
  }
  if (source.name.len == 4 && strncmp(source.name.start, "null", 4) == 0) {
    return true;
  }
  if (target.ptr_depth == source.ptr_depth &&
      target.array_dimens == source.array_dimens &&
      target.name.len == source.name.len &&
      strncmp(target.name.start, source.name.start, target.name.len) == 0) {
    return true;
  }

  if (is_explicit) {
    return true;
  }

  int t_width = 0, s_width = 0;
  bool t_signed = false, s_signed = false, t_float = false, s_float = false;

  bool t_is_num = get_numeric_info(target, &t_width, &t_signed, &t_float);
  bool s_is_num = get_numeric_info(source, &s_width, &s_signed, &s_float);

  if (t_is_num && s_is_num) {
    if (s_float && !t_float)
      return false;

    if (!t_signed && s_signed)
      return false;

    return t_width >= s_width;
  }
  return false;
}

bool resolve_imports(Arena *arena, SemCtx *sem) {
  import_relations_count = 0;

  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *m = (Module *)entry->value;
      map_free_buckets(&m->imported_mods);
      map_init(&m->imported_mods, m->mod_arena ? m->mod_arena : arena, 32);
      entry = entry->next;
    }
  }

  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *current_mod = (Module *)entry->value;
      if (!current_mod || current_mod == (Module *)1 ||
          !current_mod->ast_root) {
        entry = entry->next;
        continue;
      }
      sem_current_mod = current_mod;

      AstNode *stmt = current_mod->ast_root->as.block.first_stmt;
      while (stmt) {
        if (stmt->type == AST_USE) {
          size_t path_len = stmt->as.use_stmt.path.len;
          const char *raw_path = stmt->as.use_stmt.path.start;
          Token alias = stmt->as.use_stmt.alias;

          if (path_len > 2) {
            char *clean_rel = arena_alloc(arena, path_len - 1);
            strncpy(clean_rel, raw_path + 1, path_len - 2);
            clean_rel[path_len - 2] = '\0';
            const char *abs_import_path =
                resolve_module_path(arena, current_mod->abs_path, clean_rel);
            if (!abs_import_path) {
              sem_report(sem, DIAG_ERROR, stmt->as.use_stmt.path,
                         "Module not found: %.*s", (int)path_len, raw_path);
              return false;
            }
            Module *imported_mod = map_get(&sem->mod_cache, abs_import_path,
                                           strlen(abs_import_path));

            if (imported_mod) {
              const char *import_key = imported_mod->mod_name;
              size_t key_len = strlen(import_key);

              if (alias.len > 0) {
                import_key = alias.start;
                key_len = alias.len;
              }

              if (map_get(&current_mod->imported_mods, import_key, key_len) !=
                  NULL) {
                sem_report(sem, DIAG_ERROR, stmt->as.use_stmt.path,
                           "Error in %s at %u:%u: Duplicate mod import name "
                           "'%.*s'. Use an "
                           "'as' alias.\n",
                           current_mod->abs_path, stmt->as.use_stmt.path.line,
                           stmt->as.use_stmt.path.col, (int)key_len,
                           import_key);
                return false;
              }

              map_set(&current_mod->imported_mods, import_key, key_len,
                      imported_mod);
              record_import(imported_mod, current_mod, stmt);
            }
          }
        }
        stmt = stmt->next;
      }
      entry = entry->next;
    }
  }
  return true;
}

bool collect_mod_symbols(Arena *arena, Module *mod, SemCtx *ctx) {
  Arena *alloc_arena = mod->mod_arena ? mod->mod_arena : arena;

  AstNode *stmt = mod->ast_root->as.block.first_stmt;
  while (stmt) {
    Token name = {0};
    SymKind kind;
    bool is_valid = true;

    switch (stmt->type) {
    case AST_FUNC:
      name = stmt->as.func_def.fn_name;
      kind = SYM_FUNC;
      break;
    case AST_VAR_DECL:
      name = stmt->as.var_decl.id;
      kind = SYM_VAR;
      break;
    case AST_STRUCT:
      name = stmt->as.struct_def.structn;
      kind = SYM_STRUCT;
      break;
    case AST_UNION:
      name = stmt->as.union_def.unionn;
      kind = SYM_UNION;
      break;
    case AST_ENUM:
      name = stmt->as.enum_def.enumn;
      kind = SYM_ENUM;
      break;
    default:
      is_valid = false;
      break;
    }

    if (is_valid) {
      if (map_get(&mod->local_symbols, name.start, name.len) != NULL) {
        Sym *existing = map_get(&mod->local_symbols, name.start, name.len);

        bool is_existing_opaque =
            (existing->decl_node->type == AST_STRUCT &&
             !existing->decl_node->as.struct_def.contents) ||
            (existing->decl_node->type == AST_UNION &&
             !existing->decl_node->as.union_def.contents);

        if (!is_existing_opaque) {
          sem_report(ctx, DIAG_ERROR, name,
                     "Error: Symbol '%.*s' already defined in module %s",
                     name.len, name.start, mod->mod_name);
          return false;
        }
      }
      Sym *sym = new_sym(alloc_arena, kind, name, stmt, mod->abs_path);
      map_set(&mod->local_symbols, name.start, name.len, sym);
    }
    stmt = stmt->next;
  }
  return true;
}

Sym *scope_lookup(ScopeStack *ss, const char *key, size_t len) {
  for (int i = ss->count - 1; i >= 0; i--) {
    Sym *sym = map_get(&ss->scopes[i].symbols, key, len);
    if (sym != NULL)
      return sym;
  }
  return NULL;
}

bool scope_declare(ScopeStack *ss, Token name, Sym *symbol) {
  Scope *current_scope = &ss->scopes[ss->count - 1];

  if (map_get(&current_scope->symbols, name.start, name.len) != NULL) {
    return false;
  }

  map_set(&current_scope->symbols, name.start, name.len, symbol);
  return true;
}

void scope_stack_init(ScopeStack *ss, Arena *arena) {
  ss->arena = arena;
  ss->capacity = 16;
  ss->count = 0;
  ss->scopes = arena_alloc(arena, sizeof(Scope) * ss->capacity);
}

void push_scope(ScopeStack *ss) {
  if (ss->count >= ss->capacity) {
    size_t new_cap = ss->capacity * 2;
    Scope *new_scopes = arena_alloc(ss->arena, sizeof(Scope) * new_cap);
    memcpy(new_scopes, ss->scopes, sizeof(Scope) * ss->count);
    ss->scopes = new_scopes;
    ss->capacity = new_cap;
  }

  Scope *new_scope = &ss->scopes[ss->count++];
  map_init(&new_scope->symbols, ss->arena, 64);
}

void pop_scope(ScopeStack *ss) {
  if (ss->count > 0) {
    map_free_buckets(&ss->scopes[ss->count - 1].symbols);
    ss->count--;
  }
}

typedef struct {
  SemCtx *ctx;
  ScopeStack *ss;
  Module *mod;
  Arena *arena;
} ScopeVisitorData;

VisitResult resolve_scopes_enter(AstVisitor *visitor, AstNode *node) {
  ScopeVisitorData *data = (ScopeVisitorData *)visitor->user_data;
  SemCtx *ctx = data->ctx;
  ScopeStack *ss = data->ss;
  Module *mod = data->mod;
  Arena *arena = data->arena;

  switch (node->type) {
  case AST_PROGRAM:
  case AST_BLOCK:
  case AST_FOR:
    push_scope(ss);
    break;

  case AST_FUNC: {
    Sym *func_sym = new_sym(arena, SYM_FUNC, node->as.func_def.fn_name, node,
                            mod->abs_path);
    if (!scope_declare(ss, node->as.func_def.fn_name, func_sym)) {
      sem_report(ctx, DIAG_ERROR, node->as.func_def.fn_name,
                 "Error: Duplicate function name '%.*s'\n",
                 node->as.func_def.fn_name.len,
                 node->as.func_def.fn_name.start);
    }
    push_scope(ss);
    break;
  }

  case AST_PARAM: {
    Sym *param_sym =
        new_sym(arena, SYM_VAR, node->as.fn_param.id, node, mod->abs_path);
    if (!scope_declare(ss, node->as.fn_param.id, param_sym)) {
      sem_report(ctx, DIAG_ERROR, node->as.fn_param.id,
                 "Error: Duplicate parameter name '%.*s'\n",
                 node->as.fn_param.id.len, node->as.fn_param.id.start);
    }
    break;
  }

  case AST_VAR_DECL: {
    Sym *var_sym =
        new_sym(arena, SYM_VAR, node->as.var_decl.id, node, mod->abs_path);
    if (!scope_declare(ss, node->as.var_decl.id, var_sym)) {
      sem_report(ctx, DIAG_ERROR, node->as.var_decl.id,
                 "Error: Variable '%.*s' already declared in this scope.\n",
                 node->as.var_decl.id.len, node->as.var_decl.id.start);
    }
    break;
  }

  case AST_IDENTIF: {
    Token id = node->as.identif.val;
    Sym *found = scope_lookup(ss, id.start, id.len);
    if (!found) {
      sem_report(ctx, DIAG_ERROR, id, "Undeclared identifier '%.*s'", id.len,
                 id.start);
    } else {
      node->as.identif.res_sm = found;
    }
    break;
  }

  case AST_STRUCT:
  case AST_UNION:
  case AST_ENUM: {
    SymKind skind = (node->type == AST_STRUCT)  ? SYM_STRUCT
                    : (node->type == AST_UNION) ? SYM_UNION
                                                : SYM_ENUM;

    Token name = (node->type == AST_STRUCT)  ? node->as.struct_def.structn
                 : (node->type == AST_UNION) ? node->as.union_def.unionn
                                             : node->as.enum_def.enumn;

    Sym *sue_sym = new_sym(arena, skind, name, node, mod->abs_path);
    scope_declare(ss, name, sue_sym);

    push_scope(ss);
    Token self_tok = {.start = "self", .len = 4, .type = TOKEN_IDENTIF};
    scope_declare(ss, self_tok, sue_sym);
    break;
  }
  default:
    break;
  }

  return VISIT_CONTINUE;
}

void resolve_scopes_exit(AstVisitor *visitor, AstNode *node) {
  ScopeVisitorData *data = (ScopeVisitorData *)visitor->user_data;
  switch (node->type) {
  case AST_PROGRAM:
  case AST_BLOCK:
  case AST_FUNC:
  case AST_FOR:
  case AST_STRUCT:
  case AST_UNION:
  case AST_ENUM:
    pop_scope(data->ss);
    break;
  default:
    break;
  }
}

void resolve_scopes(Arena *arena, Module *mod, ScopeStack *ss, SemCtx *ctx) {
  if (!mod || !mod->ast_root)
    return;

  push_scope(ss);

  // Load imported module aliases into global scope
  for (size_t i = 0; i < mod->imported_mods.capacity; i++) {
    HashEntry *entry = mod->imported_mods.buckets[i];
    while (entry) {
      Module *imported_mod = (Module *)entry->value;
      Token import_tok = {
          .start = entry->key, .len = entry->key_len, .type = TOKEN_IDENTIF};
      Sym *import_sym =
          new_sym(arena, SYM_VAR, import_tok, NULL, imported_mod->abs_path);
      import_sym->is_imported_mod = true;
      scope_declare(ss, import_tok, import_sym);
      entry = entry->next;
    }
  }

  ScopeVisitorData vdata = {.ctx = ctx, .ss = ss, .mod = mod, .arena = arena};

  AstVisitor visitor = {.user_data = &vdata,
                        .enter_node = resolve_scopes_enter,
                        .exit_node = resolve_scopes_exit};

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, mod->ast_root);
  } else {
    fprintf(stderr, "OOM encountered during scope resolution.\n");
  }

  pop_scope(ss);
}

DataType create_basic_type(const char *name_str) {
  DataType t = {0};
  t.name.start = name_str;
  t.name.len = strlen(name_str);
  t.name.type = TOKEN_IDENTIF;
  return t;
}

static DataType EXPECT_BOOL = {
    .name = {.start = "bool", .len = 4, .type = TOKEN_IDENTIF}};

static Token get_expr_token(AstNode *node) {
  if (!node)
    return (Token){0};
  switch (node->type) {
  case AST_IDENTIF:
    return node->as.identif.val;
  case AST_NUM_LIT:
    return node->as.num_lit.val;
  case AST_STR_LIT:
    return node->as.str_lit.val;
  case AST_CHAR_LIT:
    return node->as.char_lit.val;
  case AST_BOOL_LIT:
    return node->as.bool_lit.val;
  case AST_NULL_LIT:
    return node->as.null_lit.val;
  case AST_BINOP:
    return get_expr_token(node->as.binop.left);
  case AST_UOP:
    return get_expr_token(node->as.unop.operand);
  case AST_ADDR_OF:
    return get_expr_token(node->as.unop.operand);
  case AST_DEREF:
    return get_expr_token(node->as.unop.operand);
  case AST_FUNC_CALL:
    return get_expr_token(node->as.func_call.caller);
  case AST_MEMBER:
    return get_expr_token(node->as.member.base);
  case AST_INDEX:
    return get_expr_token(node->as.index.base);
  case AST_CAST:
    return get_expr_token(node->as.cast.op);
  default:
    return (Token){0};
  }
}

// Check if a DataType represents a numeric type
bool is_numeric_type(DataType t) {
  if (t.ptr_depth != 0 || t.array_dimens != 0 || t.is_custom)
    return false;
  const char *s = t.name.start;
  size_t len = t.name.len;
  if (len == 4 && strncmp(s, "size", 4) == 0)
    return true;
  if (len == 4 && strncmp(s, "char", 4) == 0)
    return true;
  // Handle {i,u,f}size
  if (len == 5 && strncmp(s + 1, "size", 4) == 0) {
    char first = s[0];
    if (first == 'i' || first == 'u' || first == 'f')
      return true;
  }
  if (len < 2 || len > 3)
    return false;
  char first = s[0];
  if (first != 'i' && first != 'u' && first != 'f')
    return false;

  for (size_t i = 1; i < len; i++) {
    if (!isdigit((unsigned char)s[i]))
      return false;
  }
  return true;
}

long long parse_num_lit(AstNode *node) {
  if (node->type != AST_NUM_LIT)
    return 0;
  const char *start = node->as.num_lit.val.start;
  size_t len = node->as.num_lit.val.len;
  char buf[64] = {0};
  size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, start, copy);
  return strtoll(buf, NULL, 0);
}

bool fits_in_type(long long val, DataType t) {
  int width;
  bool is_signed, is_float;
  if (!get_numeric_info(t, &width, &is_signed, &is_float))
    return false;
  if (is_float)
    return true; // any integer can be converted to float
  if (is_signed) {
    if (width == 8)
      return val >= SCHAR_MIN && val <= SCHAR_MAX;
    if (width == 16)
      return val >= SHRT_MIN && val <= SHRT_MAX;
    if (width == 32)
      return val >= INT_MIN && val <= INT_MAX;
    if (width == 64)
      return val >= LLONG_MIN && val <= LLONG_MAX;
  } else {
    // Prevent negative signed values from passing unsigned checks
    if (val < 0)
      return false;
    unsigned long long uval = (unsigned long long)val;
    if (width == 8)
      return uval <= UCHAR_MAX;
    if (width == 16)
      return uval <= USHRT_MAX;
    if (width == 32)
      return uval <= UINT_MAX;
    if (width == 64)
      return uval <= ULLONG_MAX;
  }
  return false;
}

DataType common_numeric_type(DataType a, DataType b) {
  int w1, w2;
  bool s1, s2, f1, f2;
  get_numeric_info(a, &w1, &s1, &f1);
  get_numeric_info(b, &w2, &s2, &f2);
  // prefer larger width, if equal, prefer signed
  if (w1 >= w2)
    return a;
  return b;
}

typedef struct {
  SemCtx *ctx;
  Arena *arena;
  AstNode *func_stack[64];
  int func_top;
  HashMap exp_map;
} TCData;

void set_expected(HashMap *map, AstNode *n, DataType *exp, Arena *arena) {
  if (n && exp) {
    AstNode **key_ptr = arena_alloc(arena, sizeof(AstNode *));
    *key_ptr = n;
    map_set(map, (const char *)key_ptr, sizeof(AstNode *), exp);
  }
}

DataType *get_expected(HashMap *map, AstNode *n) {
  return (DataType *)map_get(map, (const char *)&n, sizeof(AstNode *));
}

VisitResult tc_enter(AstVisitor *visitor, AstNode *n) {
  TCData *data = visitor->user_data;

  if (n->type == AST_FUNC) {
    data->func_stack[data->func_top++] = n;
  }

  bool is_top_level = (data->func_top == 0 ||
                       (data->func_top == 1 && n == data->func_stack[0]));

  if (is_top_level) {
    if (!n->is_dirty && (n->type == AST_FUNC || n->type == AST_VAR_DECL ||
                         n->type == AST_STRUCT || n->type == AST_UNION ||
                         n->type == AST_ENUM)) {
      // Clean top level block nodes should skip digging but process synthesis
      // in exit
      return VISIT_SKIP_CHILDREN;
    }
    if (n->is_dirty) {
      n->is_dirty = false;
    }
  }

  DataType *expected = get_expected(&data->exp_map, n);

  switch (n->type) {
  case AST_VAR_DECL:
    set_expected(&data->exp_map, n->as.var_decl.init, &n->as.var_decl.type, data->arena);
    break;
  case AST_BINOP: {
    DataType *exp = expected;
    if (n->as.binop.op.type == TOKEN_COMPARE)
      exp = NULL;
    set_expected(&data->exp_map, n->as.binop.left, exp, data->arena);
    set_expected(&data->exp_map, n->as.binop.right, exp, data->arena);
    break;
  }
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF: {
    if (expected) {
      DataType *inner = arena_alloc(data->arena, sizeof(DataType));
      *inner = *expected;
      if (n->type == AST_ADDR_OF)
        inner->ptr_depth--;
      else if (n->type == AST_DEREF)
        inner->ptr_depth++;
      set_expected(&data->exp_map, n->as.unop.operand, inner, data->arena);
    }
    break;
  }
  case AST_IF:
    set_expected(&data->exp_map, n->as.if_check.check, &EXPECT_BOOL, data->arena);
    set_expected(&data->exp_map, n->as.if_check.action, expected, data->arena);
    set_expected(&data->exp_map, n->as.if_check.elseAct, expected, data->arena);
    break;
  case AST_WHILE:
    set_expected(&data->exp_map, n->as.while_loop.check, &EXPECT_BOOL, data->arena);
    break;
  case AST_FOR:
    set_expected(&data->exp_map, n->as.for_loop.check, &EXPECT_BOOL, data->arena);
    break;
  case AST_FUNC_CALL: {
    AstNode *fn_decl = NULL;
    if (n->as.func_call.caller && n->as.func_call.caller->type == AST_IDENTIF &&
        n->as.func_call.caller->as.identif.res_sm) {
      fn_decl = n->as.func_call.caller->as.identif.res_sm->decl_node;
    }
    AstNode *curr_arg = n->as.func_call.args;
    AstNode *curr_param = NULL;
    if (fn_decl && n->as.func_call.caller->type == AST_IDENTIF &&
        n->as.func_call.caller->as.identif.res_sm->kind == SYM_FUNC) {
      curr_param = fn_decl->as.func_def.params;
    }
    while (curr_arg) {
      if (curr_param) {
        set_expected(&data->exp_map, curr_arg, &curr_param->as.fn_param.type, data->arena);
        curr_param = curr_param->next;
      }
      curr_arg = curr_arg->next;
    }
    break;
  }
  case AST_CAST:
    set_expected(&data->exp_map, n->as.cast.op, &n->as.cast.target, data->arena);
    break;
  case AST_RET: {
    AstNode *curr_func =
        (data->func_top > 0) ? data->func_stack[data->func_top - 1] : NULL;
    if (curr_func && n->as.ret_stmt.expr) {
      set_expected(&data->exp_map, n->as.ret_stmt.expr,
                   &curr_func->as.func_def.ret_type, data->arena);
    }
    break;
  }
  case AST_ARRAY_LIT: {
    AstNode *curr = n->as.array_lit.elements;
    while (curr) {
      set_expected(&data->exp_map, curr, expected, data->arena);
      curr = curr->next;
    }
    break;
  }
  default:
    break;
  }

  return VISIT_CONTINUE;
}

void tc_exit(AstVisitor *visitor, AstNode *n) {
  TCData *data = visitor->user_data;
  DataType *expected = get_expected(&data->exp_map, n);
  SemCtx *ctx = data->ctx;
  AstNode *curr_func =
      (data->func_top > 0) ? data->func_stack[data->func_top - 1] : NULL;

  switch (n->type) {
  case AST_NUM_LIT: {
    if (expected) {
      n->eval_type = *expected;
    } else {
      const char *val_str = n->as.num_lit.val.start;
      int len = n->as.num_lit.val.len;

      bool is_float = false;
      for (int i = 0; i < len; i++) {
        if (val_str[i] == '.') {
          is_float = true;
          break;
        }
      }

      if (is_float) {
        n->eval_type = create_basic_type("f32");
      } else {
        char buf[64] = {0};
        int copy_len = len < 63 ? len : 63;
        strncpy(buf, val_str, copy_len);

        long long val = strtoll(buf, NULL, 0);

        if (val >= -128 && val <= 127)
          n->eval_type = create_basic_type("i8");
        else if (val >= -32768 && val <= 32767)
          n->eval_type = create_basic_type("i16");
        else if (val >= -2147483648LL && val <= 2147483647LL)
          n->eval_type = create_basic_type("i32");
        else
          n->eval_type = create_basic_type("i64");
      }
    }
    break;
  }
  case AST_STR_LIT:
    n->eval_type = create_basic_type("str");
    break;
  case AST_BOOL_LIT:
    n->eval_type = create_basic_type("bool");
    break;
  case AST_CHAR_LIT:
    n->eval_type = create_basic_type("char");
    break;
  case AST_NULL_LIT:
    n->eval_type = create_basic_type("null");
    break;
  case AST_IDENTIF:
    if (n->as.identif.res_sm) {
      Sym *sym = n->as.identif.res_sm;
      if (sym->kind == SYM_VAR && sym->decl_node) {
        if (sym->decl_node->type == AST_PARAM)
          n->eval_type = sym->decl_node->as.fn_param.type;
        else
          n->eval_type = sym->decl_node->as.var_decl.type;
      } else if (sym->kind == SYM_FUNC && sym->decl_node) {
        n->eval_type = sym->decl_node->as.func_def.ret_type;
      } else if (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION ||
                 sym->kind == SYM_ENUM) {
        n->eval_type = create_basic_type("");
        n->eval_type.name = sym->name;
        n->eval_type.is_custom = true;
      }
    }
    break;
  case AST_VAR_DECL:
    if (n->as.var_decl.init) {
      if (!is_type_compatible(n->as.var_decl.type,
                              n->as.var_decl.init->eval_type, false)) {
        sem_report(ctx, DIAG_WARNING, n->as.var_decl.id,
                   "Incompatible assignment for variable '%.*s'",
                   n->as.var_decl.id.len, n->as.var_decl.id.start);
      }
    }
    break;
  case AST_BINOP: {
    if (!n->as.binop.left || !n->as.binop.right)
      break;

    AstNode *left = n->as.binop.left;
    AstNode *right = n->as.binop.right;

    DataType left_t = left->eval_type;
    DataType right_t = right->eval_type;

    bool left_lit = (left->type == AST_NUM_LIT);
    bool right_lit = (right->type == AST_NUM_LIT);

    if (left_lit && is_numeric_type(right_t)) {
      long long val = parse_num_lit(left);
      if (fits_in_type(val, right_t)) {
        left->eval_type = right_t;
        left_t = right_t;
      } else {
        sem_report(ctx, DIAG_ERROR, left->as.num_lit.val,
                   "Integer literal %lld does not fit in type '%.*s'", val,
                   (int)right_t.name.len, right_t.name.start);
        left->eval_type = right_t;
        left_t = right_t;
      }
    } else if (right_lit && is_numeric_type(left_t)) {
      long long val = parse_num_lit(right);
      if (fits_in_type(val, left_t)) {
        right->eval_type = left_t;
        right_t = left_t;
      } else {
        sem_report(ctx, DIAG_ERROR, right->as.num_lit.val,
                   "Integer literal %lld does not fit in type '%.*s'", val,
                   (int)left_t.name.len, left_t.name.start);
        right->eval_type = left_t;
        right_t = left_t;
      }
    }

    if (left_lit && right_lit) {
      if (is_numeric_type(left_t) && is_numeric_type(right_t)) {
        if (expected && is_numeric_type(*expected)) {
          long long lval = parse_num_lit(left);
          long long rval = parse_num_lit(right);
          if (fits_in_type(lval, *expected) && fits_in_type(rval, *expected)) {
            left->eval_type = *expected;
            right->eval_type = *expected;
            left_t = *expected;
            right_t = *expected;
          } else {
            DataType common = common_numeric_type(left_t, right_t);
            left->eval_type = common;
            right->eval_type = common;
            left_t = common;
            right_t = common;
          }
        } else {
          DataType common = common_numeric_type(left_t, right_t);
          left->eval_type = common;
          right->eval_type = common;
          left_t = common;
          right_t = common;
        }
      }
    }

    bool is_assign = false;
    Token op = n->as.binop.op;
    if (op.type == TOKEN_ASSIGN) {
      is_assign = true;
    } else if (op.len == 2) {
      const char *s = op.start;
      if ((s[0] == '+' || s[0] == '-' || s[0] == '*' || s[0] == '/' ||
           s[0] == '%' || s[0] == '&' || s[0] == '|' || s[0] == '^') &&
          s[1] == '=') {
        is_assign = true;
      }
    } else if (op.len == 3) {
      const char *s = op.start;
      if ((s[0] == '<' && s[1] == '<' && s[2] == '=') ||
          (s[0] == '>' && s[1] == '>' && s[2] == '=')) {
        is_assign = true;
      }
    }

    if (is_assign && !left_t.is_mut) {
      Token err_tok = get_expr_token(left);
      if (err_tok.len == 0)
        err_tok = op;
      sem_report(ctx, DIAG_ERROR, err_tok,
                 "Cannot mutate immutable variable '%.*s'", err_tok.len,
                 err_tok.start);
    }

    if (op.start[0] == '/' && right->type == AST_NUM_LIT) {
      if (atof(right->as.num_lit.val.start) == 0.0) {
        sem_report(ctx, DIAG_ERROR, right->as.num_lit.val, "Division by zero.");
      }
    }

    if (op.type == TOKEN_COMPARE) {
      bool left_null =
          (left_t.name.len == 4 && strncmp(left_t.name.start, "null", 4) == 0);
      bool right_null = (right_t.name.len == 4 &&
                         strncmp(right_t.name.start, "null", 4) == 0);
      bool left_ptr = (left_t.ptr_depth > 0 || left_t.array_dimens > 0);
      bool right_ptr = (right_t.ptr_depth > 0 || right_t.array_dimens > 0);

      bool valid_compare = false;
      if (left_null && right_null) {
        valid_compare = true;
      } else if (left_null && right_ptr) {
        valid_compare = true;
      } else if (right_null && left_ptr) {
        valid_compare = true;
      } else {
        int l_w, r_w;
        bool l_s, r_s, l_f, r_f;
        bool left_num = get_numeric_info(left_t, &l_w, &l_s, &l_f);
        bool right_num = get_numeric_info(right_t, &r_w, &r_s, &r_f);
        bool ptr_ok = (left_t.ptr_depth == right_t.ptr_depth &&
                       left_t.array_dimens == right_t.array_dimens);
        if ((left_num && right_num) || ptr_ok) {
          valid_compare = true;
        }
      }

      if (!valid_compare) {
        sem_report(ctx, DIAG_ERROR, op,
                   "Cannot compare non‑numeric types '%.*s' and '%.*s'",
                   left_t.name.len, left_t.name.start, right_t.name.len,
                   right_t.name.start);
      }
      n->eval_type = EXPECT_BOOL;
    } else {
      bool left_is_ptr =
          left_t.ptr_depth > 0 ||
          (left_t.name.len == 3 && strncmp(left_t.name.start, "str", 3) == 0 &&
           left_t.ptr_depth == 0);
      bool right_is_ptr = right_t.ptr_depth > 0 ||
                          (right_t.name.len == 3 &&
                           strncmp(right_t.name.start, "str", 3) == 0 &&
                           right_t.ptr_depth == 0);

      bool is_ptr_arithmetic = false;
      if ((op.len == 1 && (op.start[0] == '+' || op.start[0] == '-')) ||
          (op.len == 2 && ((op.start[0] == '+' && op.start[1] == '=') ||
                           (op.start[0] == '-' && op.start[1] == '=')))) {
        if ((left_is_ptr && is_numeric_type(right_t)) ||
            (right_is_ptr && is_numeric_type(left_t))) {
          is_ptr_arithmetic = true;
        }
      }

      if (!is_ptr_arithmetic && !is_type_compatible(left_t, right_t, false) &&
          !is_type_compatible(right_t, left_t, false)) {
        sem_report(ctx, DIAG_WARNING, op,
                   "Incompatible operands '%.*s' and '%.*s' for '%.*s'",
                   left_t.name.len, left_t.name.start, right_t.name.len,
                   right_t.name.start, op.len, op.start);
      }

      if (expected) {
        n->eval_type = *expected;
      } else {
        if (is_ptr_arithmetic) {
          n->eval_type = left_is_ptr ? left_t : right_t;
        } else if (is_numeric_type(left_t) && is_numeric_type(right_t)) {
          int l_w = 0, r_w = 0;
          bool l_s, r_s, l_f, r_f;
          get_numeric_info(left_t, &l_w, &l_s, &l_f);
          get_numeric_info(right_t, &r_w, &r_s, &r_f);
          n->eval_type = (l_w >= r_w) ? left_t : right_t;
        } else {
          n->eval_type = left_t;
        }
      }
    }
    break;
  }
  case AST_CAST:
    if (!is_type_compatible(n->as.cast.target, n->as.cast.op->eval_type,
                            true)) {
      sem_report(ctx, DIAG_ERROR, n->as.cast.target.name,
                 "Type Error at %u:%u: Invalid explicit cast.\n",
                 n->as.cast.target.name.line, n->as.cast.target.name.col);
    }
    n->eval_type = n->as.cast.target;
    break;
  case AST_RET:
    if (curr_func && n->as.ret_stmt.expr) {
      if (!is_type_compatible(curr_func->as.func_def.ret_type,
                              n->as.ret_stmt.expr->eval_type, false)) {
        sem_report(ctx, DIAG_ERROR, n->as.ret_stmt.ret_kw,
                   "Type Error at %u:%u: Function return type mismatch.\n",
                   n->as.ret_stmt.ret_kw.line, n->as.ret_stmt.ret_kw.col);
      }
    }
    break;
  case AST_FUNC_CALL:
    if (n->as.func_call.caller && n->as.func_call.caller->type == AST_IDENTIF &&
        n->as.func_call.caller->as.identif.res_sm &&
        n->as.func_call.caller->as.identif.res_sm->decl_node) {

      Sym *sym = n->as.func_call.caller->as.identif.res_sm;

      if (sym->kind == SYM_FUNC) {
        n->eval_type = sym->decl_node->as.func_def.ret_type;
      } else if (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION) {
        n->eval_type.name = sym->name;
        n->eval_type.is_custom = true;
      } else {
        n->eval_type = create_basic_type("any");
      }

      if (sym->kind == SYM_FUNC) {
        AstNode *func_decl = sym->decl_node;
        AstNode *param = func_decl->as.func_def.params;
        AstNode *arg = n->as.func_call.args;

        int arg_count = 0, param_count = 0;
        for (AstNode *a = arg; a; a = a->next)
          arg_count++;
        for (AstNode *p = param; p; p = p->next)
          param_count++;

        if (arg_count != param_count) {
          sem_report(ctx, DIAG_ERROR, n->as.func_call.caller->as.identif.val,
                     "Function '%.*s' expects %d argument(s), got %d",
                     n->as.func_call.caller->as.identif.val.len,
                     n->as.func_call.caller->as.identif.val.start, param_count,
                     arg_count);
        } else {
          param = func_decl->as.func_def.params;
          arg = n->as.func_call.args;
          while (arg && param) {
            DataType expected_t = param->as.fn_param.type;
            DataType actual_t = arg->eval_type;
            if (!is_type_compatible(expected_t, actual_t, false)) {
              Token err_tok = get_expr_token(arg);
              if (err_tok.len == 0)
                err_tok = n->as.func_call.caller->as.identif.val;
              sem_report(ctx, DIAG_ERROR, err_tok,
                         "Argument type mismatch: expected '%.*s', got '%.*s'",
                         expected_t.name.len, expected_t.name.start,
                         actual_t.name.len, actual_t.name.start);
            }
            arg = arg->next;
            param = param->next;
          }
        }
      }
    } else if (n->as.func_call.caller->type == AST_MEMBER) {
      n->eval_type = create_basic_type("any");
    }
    break;
  case AST_ARRAY_LIT:
    if (expected) {
      n->eval_type = *expected;
    } else if (n->as.array_lit.elements) {
      n->eval_type = n->as.array_lit.elements->eval_type;
      n->eval_type.array_dimens++;
    }
    break;
  case AST_BLOCK: {
    AstNode *last = n->as.block.first_stmt;
    while (last && last->next) {
      last = last->next;
    }
    if (last) {
      n->eval_type = last->eval_type;
    }
    break;
  }
  case AST_IF:
    if (n->as.if_check.action) {
      n->eval_type = n->as.if_check.action->eval_type;
    }
    break;
  case AST_INDEX:
    if (n->as.index.base) {
      DataType base_type = n->as.index.base->eval_type;
      n->eval_type = base_type;

      if (base_type.array_dimens > 0) {
        n->eval_type.array_dimens--;
      } else if (base_type.ptr_depth > 0) {
        n->eval_type.ptr_depth--;
      } else if (base_type.name.len == 3 &&
                 strncmp(base_type.name.start, "str", 3) == 0) {
        DataType char_type = create_basic_type("char");
        char_type.is_mut = base_type.is_mut;
        char_type.is_static = base_type.is_static;
        char_type.is_extern = base_type.is_extern;
        char_type.is_threadlocal = base_type.is_threadlocal;
        n->eval_type = char_type;
      }

      if (n->as.index.index) {
        DataType idx_type = n->as.index.index->eval_type;
        if (!is_numeric_type(idx_type)) {
          sem_report(ctx, DIAG_ERROR, get_expr_token(n->as.index.index),
                     "Index must be of integer type, got '%.*s'",
                     idx_type.name.len, idx_type.name.start);
        }
      }
    }
    break;
  case AST_ADDR_OF:
    if (n->as.unop.operand) {
      n->eval_type = n->as.unop.operand->eval_type;
      n->eval_type.ptr_depth++;
    }
    break;
  case AST_DEREF:
    if (n->as.unop.operand) {
      DataType base_type = n->as.unop.operand->eval_type;

      if (base_type.ptr_depth == 0 && base_type.array_dimens == 0 &&
          base_type.name.len == 3 &&
          strncmp(base_type.name.start, "str", 3) == 0) {

        DataType char_type = create_basic_type("char");
        char_type.is_mut = base_type.is_mut;
        char_type.is_static = base_type.is_static;
        char_type.is_extern = base_type.is_extern;
        char_type.is_threadlocal = base_type.is_threadlocal;
        n->eval_type = char_type;

      } else {
        n->eval_type = base_type;
        n->eval_type.ptr_depth--;
      }
    }
    break;
  case AST_UOP:
    if (n->as.unop.operand) {
      n->eval_type = n->as.unop.operand->eval_type;
    }
    Token op_tok = n->as.unop.op;
    if (op_tok.len == 2 && (strncmp(op_tok.start, "++", 2) == 0 ||
                            strncmp(op_tok.start, "--", 2) == 0)) {
      if (!n->eval_type.is_mut) {
        Token err_tok = get_expr_token(n->as.unop.operand);
        if (err_tok.len == 0)
          err_tok = op_tok;
        sem_report(ctx, DIAG_ERROR, err_tok,
                   "Cannot mutate immutable variable '%.*s' using '%.*s'",
                   err_tok.len, err_tok.start, op_tok.len, op_tok.start);
      }
    }
    break;
  case AST_MEMBER: {
    if (n->as.member.base) {
      DataType base_t = n->as.member.base->eval_type;
      if (base_t.name.len > 0) {
        n->as.member.type = base_t.name;
      }
      n->eval_type = create_basic_type("any");
      n->eval_type.is_mut = base_t.is_mut;
    }
    break;
  }
  case AST_SIZEOF:
    n->eval_type = create_basic_type("size");
    break;
  case AST_ERROR:
    n->eval_type = create_basic_type("any");
    break;
  default:
    break;
  }

  if (n->type == AST_FUNC) {
    data->func_top--;
  }
}

void type_check_ast(Arena *arena, AstNode *root, SemCtx *ctx) {
  if (!root)
    return;

  TCData data = {0};
  data.ctx = ctx;
  data.arena = arena;
  map_init(&data.exp_map, arena, 2048);

  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = tc_enter;
  visitor.exit_node = tc_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered during type checking.\n");
  }

  map_free_buckets(&data.exp_map);
}
