#include "sem_types.h"
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Module *sem_current_mod = NULL;
Module *sem_main_mod = NULL;

typedef struct {
  Module *mod;
  Module *parent_mod;
  AstNode *use_stmt;
} ImportRelation;

#define MAX_IMPORT_RELATIONS 1024
static ImportRelation import_relations[MAX_IMPORT_RELATIONS];
static int import_relations_count = 0;

void record_import(Module *mod, Module *parent_mod, AstNode *use_stmt) {
  for (int i = 0; i < import_relations_count; i++) {
    if (import_relations[i].mod == mod) {
      return;
    }
  }
  if (import_relations_count < MAX_IMPORT_RELATIONS) {
    import_relations[import_relations_count++] = (ImportRelation){mod, parent_mod, use_stmt};
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

  const char *report_file = NULL;
  int report_line = token.line;
  int report_col = token.col;
  int report_len = (int)token.len;

  // Determine the file/line context based on inclusion depth
  if (sem_current_mod != NULL) {
    if (sem_current_mod == sem_main_mod) {
      report_file = sem_current_mod->abs_path;
    } else {
      // Its a dependency module so report error at its corresponding 'use' statement context
      ImportRelation *rel = get_import_relation(sem_current_mod);
      if (rel && rel->parent_mod && rel->use_stmt) {
        report_file = rel->parent_mod->abs_path;
        report_line = rel->use_stmt->as.use_stmt.path.line;
        report_col = rel->use_stmt->as.use_stmt.path.col;
        report_len = (int)rel->use_stmt->as.use_stmt.path.len;
      } else {
        report_file = sem_current_mod->abs_path;
      }
    }
  }

  if (ctx && ctx->diags) {
    diaglist_add(ctx->diags, sev, msg, report_file, report_line, report_col, report_line,
                 report_col + report_len);
    free(msg);
  } else {
    if (report_file) {
      fprintf(stderr, "%s:%d:%d: ", report_file, report_line, report_col);
    }
    fprintf(stderr, "%s\n", msg);
    free(msg);
  }
}

void sem_init(SemCtx *ctx, Arena *arena) {
  ctx->arena = arena;
  map_init(&ctx->mod_cache, arena, 1024);
  ctx->std_lib_env_path = getenv("TX_LIB_SEARCH_PATH");
}

void sem_deinit(SemCtx *ctx) { map_free_buckets(&ctx->mod_cache); }

Sym *new_sym(Arena *arena, SymKind kind, Token name, AstNode *decl, const char *fpath) {
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

  map_init(&m->local_symbols, arena, 128);
  map_init(&m->imported_mods, arena, 32);
  return m;
}

bool get_numeric_info(DataType t, int *width, bool *is_signed, bool *is_float) {
  if (t.ptr_depth > 0 || t.array_dimens > 0 || t.is_custom)
    return false;
  if (t.name.len < 2)
    return false;

  char kind = t.name.start[0];
  if (kind == 'u' || kind == 'i' || kind == 'f') {
    *is_signed = (kind == 'i' || kind == 'f');
    *is_float = (kind == 'f');
    *width = atoi(t.name.start + 1);
    return true;
  }
  return false;
}

bool is_type_compatible(DataType target, DataType source, bool is_explicit,
                        SemCtx *ctx) {
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

  sem_report(
      ctx, DIAG_ERROR, target.name,
      "Types %.*s (ptr_depth: %ld) and %.*s (ptr_depth: %ld) are not "
      "compatible (safely) at %u:%u, try explicit casting to get around this\n",
      (int)target.name.len, target.name.start, target.ptr_depth,
      (int)source.name.len, source.name.start, source.ptr_depth,
      target.name.line, target.name.col);
  return false;
}

bool resolve_imports(Arena *arena, SemCtx *sem) {
  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *current_mod = (Module *)entry->value;
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
            const char *abs_import_path = resolve_alloc(arena, clean_rel);

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
        sem_report(ctx, DIAG_ERROR, name,
                   "Error: Symbol '%.*s' already defined in module %s",
                   name.len, name.start, mod->mod_name);
        return false;
      }

      if (map_get(&mod->imported_mods, name.start, name.len) != NULL) {
        sem_report(ctx, DIAG_ERROR, name,
                   "Error: Symbol '%.*s' conflicts with an imported module or "
                   "alias in mod %s\n",
                   name.len, name.start, mod->mod_name);
        return false;
      }
      Sym *sym = new_sym(arena, kind, name, stmt, mod->abs_path);
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

void resolve_scopes(Arena *arena, Module *mod, ScopeStack *ss, SemCtx *ctx) {
  if (!mod || !mod->ast_root)
    return;

  AstNode *root = mod->ast_root;

  size_t stack_cap = 1024;
  TravItem *stack = malloc(sizeof(TravItem) * stack_cap);
  size_t top = 0;

  stack[top++] = (TravItem){root, ACTION_VISIT_NODE};

  push_scope(ss);

  for (size_t i = 0; i < mod->imported_mods.capacity; i++) {
    HashEntry *entry = mod->imported_mods.buckets[i];
    while (entry) {
      Token import_tok = {
          .start = entry->key, .len = entry->key_len, .type = TOKEN_IDENTIF};
      Sym *import_sym = new_sym(arena, SYM_VAR, import_tok, NULL, mod->abs_path);
      import_sym->is_imported_mod = true;
      scope_declare(ss, import_tok, import_sym);
      entry = entry->next;
    }
  }

#define PUSH_TRAV(n, act)                                                      \
  do {                                                                         \
    if (top >= stack_cap) {                                                    \
      stack_cap *= 2;                                                          \
      stack = realloc(stack, sizeof(TravItem) * stack_cap);                    \
    }                                                                          \
    stack[top++] = (TravItem){n, act};                                         \
  } while (0)

#define PUSH_LL_REVERSE(head, act)                                             \
  do {                                                                         \
    AstNode *_curr = (head);                                                   \
    size_t _cnt = 0;                                                           \
    while (_curr) {                                                            \
      _cnt++;                                                                  \
      _curr = _curr->next;                                                     \
    }                                                                          \
    if (_cnt > 0) {                                                            \
      AstNode **_arr = malloc(sizeof(AstNode *) * _cnt);                       \
      _curr = (head);                                                          \
      for (size_t _i = 0; _i < _cnt; _i++) {                                   \
        _arr[_i] = _curr;                                                      \
        _curr = _curr->next;                                                   \
      }                                                                        \
      for (int _i = (int)_cnt - 1; _i >= 0; _i--) {                            \
        PUSH_TRAV(_arr[_i], act);                                              \
      }                                                                        \
      free(_arr);                                                              \
    }                                                                          \
  } while (0)

  while (top > 0) {
    TravItem item = stack[--top];

    if (item.action == ACTION_POP_SCOPE) {
      pop_scope(ss);
      continue;
    }

    AstNode *node = item.node;
    if (!node)
      continue;

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK: {
      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);
      PUSH_LL_REVERSE(node->as.block.first_stmt, ACTION_VISIT_NODE);
      break;
    }

    case AST_FUNC: {
      Sym *func_sym = new_sym(arena, SYM_FUNC, node->as.func_def.fn_name, node, mod->abs_path);
      if (!scope_declare(ss, node->as.func_def.fn_name, func_sym)) {
        sem_report(ctx, DIAG_ERROR, node->as.func_def.fn_name,
                   "Error: Duplicate function name '%.*s'\n",
                   node->as.func_def.fn_name.len,
                   node->as.func_def.fn_name.start);
      }

      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);

      if (node->as.func_def.block) {
        PUSH_TRAV(node->as.func_def.block, ACTION_VISIT_NODE);
      }

      PUSH_LL_REVERSE(node->as.func_def.params, ACTION_VISIT_NODE);
      break;
    }

    case AST_PARAM: {
      Sym *param_sym = new_sym(arena, SYM_VAR, node->as.fn_param.id, node, mod->abs_path);
      if (!scope_declare(ss, node->as.fn_param.id, param_sym)) {
        sem_report(ctx, DIAG_ERROR, node->as.fn_param.id,
                   "Error: Duplicate parameter name '%.*s'\n",
                   node->as.fn_param.id.len, node->as.fn_param.id.start);
      }
      break;
    }

    case AST_VAR_DECL: {
      if (node->as.var_decl.init) {
        PUSH_TRAV(node->as.var_decl.init, ACTION_VISIT_NODE);
      }

      Sym *var_sym = new_sym(arena, SYM_VAR, node->as.var_decl.id, node, mod->abs_path);
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

    case AST_BINOP: {
      PUSH_TRAV(node->as.binop.right, ACTION_VISIT_NODE);
      PUSH_TRAV(node->as.binop.left, ACTION_VISIT_NODE);
      break;
    }

    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF: {
      if (node->as.unop.operand) {
        PUSH_TRAV(node->as.unop.operand, ACTION_VISIT_NODE);
      }
      break;
    }

    case AST_IF: {
      if (node->as.if_check.elseAct)
        PUSH_TRAV(node->as.if_check.elseAct, ACTION_VISIT_NODE);
      if (node->as.if_check.action)
        PUSH_TRAV(node->as.if_check.action, ACTION_VISIT_NODE);
      if (node->as.if_check.check)
        PUSH_TRAV(node->as.if_check.check, ACTION_VISIT_NODE);
      break;
    }

    case AST_WHILE: {
      if (node->as.while_loop.action)
        PUSH_TRAV(node->as.while_loop.action, ACTION_VISIT_NODE);
      if (node->as.while_loop.check)
        PUSH_TRAV(node->as.while_loop.check, ACTION_VISIT_NODE);
      break;
    }

    case AST_FOR: {
      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);

      if (node->as.for_loop.action)
        PUSH_TRAV(node->as.for_loop.action, ACTION_VISIT_NODE);
      if (node->as.for_loop.inc)
        PUSH_TRAV(node->as.for_loop.inc, ACTION_VISIT_NODE);
      if (node->as.for_loop.check)
        PUSH_TRAV(node->as.for_loop.check, ACTION_VISIT_NODE);
      if (node->as.for_loop.init)
        PUSH_TRAV(node->as.for_loop.init, ACTION_VISIT_NODE);
      break;
    }

    case AST_FUNC_CALL: {
      PUSH_LL_REVERSE(node->as.func_call.args, ACTION_VISIT_NODE);
      if (node->as.func_call.caller)
        PUSH_TRAV(node->as.func_call.caller, ACTION_VISIT_NODE);
      break;
    }

    case AST_ARRAY_LIT: {
      PUSH_LL_REVERSE(node->as.array_lit.elements, ACTION_VISIT_NODE);
      break;
    }

    case AST_INDEX: {
      if (node->as.index.index)
        PUSH_TRAV(node->as.index.index, ACTION_VISIT_NODE);
      if (node->as.index.base)
        PUSH_TRAV(node->as.index.base, ACTION_VISIT_NODE);
      break;
    }

    case AST_MEMBER: {
      if (node->as.member.base)
        PUSH_TRAV(node->as.member.base, ACTION_VISIT_NODE);
      break;
    }

    case AST_RET: {
      if (node->as.ret_stmt.expr)
        PUSH_TRAV(node->as.ret_stmt.expr, ACTION_VISIT_NODE);
      break;
    }

    case AST_DEFER: {
      if (node->as.defer_stmt.contents)
        PUSH_TRAV(node->as.defer_stmt.contents, ACTION_VISIT_NODE);
      break;
    }

    case AST_SWITCH: {
      if (node->as.switch_stmt.default_case)
        PUSH_TRAV(node->as.switch_stmt.default_case, ACTION_VISIT_NODE);
      PUSH_LL_REVERSE(node->as.switch_stmt.cases, ACTION_VISIT_NODE);
      if (node->as.switch_stmt.check)
        PUSH_TRAV(node->as.switch_stmt.check, ACTION_VISIT_NODE);
      break;
    }

    case AST_CASE: {
      if (node->as.case_stmt.action)
        PUSH_TRAV(node->as.case_stmt.action, ACTION_VISIT_NODE);
      if (node->as.case_stmt.val)
        PUSH_TRAV(node->as.case_stmt.val, ACTION_VISIT_NODE);
      break;
    }

    case AST_STRUCT: {
      Sym *struct_sym =
          new_sym(arena, SYM_STRUCT, node->as.struct_def.structn, node, mod->abs_path);
      scope_declare(ss, node->as.struct_def.structn, struct_sym);

      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);

      Token self_tok = {.start = "self", .len = 4, .type = TOKEN_IDENTIF};
      scope_declare(ss, self_tok, struct_sym);

      PUSH_LL_REVERSE(node->as.struct_def.contents, ACTION_VISIT_NODE);
      break;
    }

    case AST_UNION: {
      Sym *union_sym =
          new_sym(arena, SYM_UNION, node->as.union_def.unionn, node, mod->abs_path);
      scope_declare(ss, node->as.union_def.unionn, union_sym);

      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);

      Token self_tok = {.start = "self", .len = 4, .type = TOKEN_IDENTIF};
      scope_declare(ss, self_tok, union_sym);

      PUSH_LL_REVERSE(node->as.union_def.contents, ACTION_VISIT_NODE);
      break;
    }

    case AST_ENUM: {
      Sym *enum_sym = new_sym(arena, SYM_ENUM, node->as.enum_def.enumn, node, mod->abs_path);
      scope_declare(ss, node->as.enum_def.enumn, enum_sym);

      push_scope(ss);
      PUSH_TRAV(NULL, ACTION_POP_SCOPE);

      Token self_tok = {.start = "self", .len = 4, .type = TOKEN_IDENTIF};
      scope_declare(ss, self_tok, enum_sym);

      PUSH_LL_REVERSE(node->as.enum_def.contents, ACTION_VISIT_NODE);
      break;
    }

    case AST_EXTERN: {
      PUSH_LL_REVERSE(node->as.extern_block.contents, ACTION_VISIT_NODE);
      break;
    }

    case AST_CAST: {
      if (node->as.cast.op) {
        PUSH_TRAV(node->as.cast.op, ACTION_VISIT_NODE);
      }
      break;
    }

    default:
      break;
    }
  }

#undef PUSH_TRAV
#undef PUSH_LL_REVERSE

  pop_scope(ss);
  free(stack);
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

void type_check_ast(Arena *arena, AstNode *root, SemCtx *ctx) {
  if (!root)
    return;

  size_t cap = 1024;
  TCItem *stack = malloc(sizeof(TCItem) * cap);
  size_t top = 0;

  stack[top++] = (TCItem){root, TC_VISIT_CHILDREN, NULL, NULL};

  while (top > 0) {
    TCItem item = stack[--top];
    AstNode *node = item.node;
    if (!node)
      continue;
    DataType *expected = item.expected;

    if (item.state == TC_VISIT_CHILDREN) {
      if (top >= cap - 32) {
        cap *= 2;
        stack = realloc(stack, sizeof(TCItem) * cap);
      }

      stack[top++] = (TCItem){node, TC_EVAL_NODE, expected, item.curr_func};

      switch (node->type) {
      case AST_PROGRAM:
      case AST_BLOCK: {
        AstNode *curr = node->as.block.first_stmt;
        size_t count = 0;
        while (curr) {
          count++;
          curr = curr->next;
        }
        if (count > 0) {
          AstNode **arr = malloc(sizeof(AstNode *) * count);
          curr = node->as.block.first_stmt;
          for (size_t i = 0; i < count; i++) {
            arr[i] = curr;
            curr = curr->next;
          }
          for (int i = count - 1; i >= 0; i--) {
            stack[top++] =
                (TCItem){arr[i], TC_VISIT_CHILDREN, NULL, item.curr_func};
          }
          free(arr);
        }
        break;
      }
      case AST_FUNC:
        if (node->as.func_def.block) {
          stack[top++] =
              (TCItem){node->as.func_def.block, TC_VISIT_CHILDREN, NULL, node};
        }
        break;
      case AST_VAR_DECL:
        stack[top++] = (TCItem){node->as.var_decl.init, TC_VISIT_CHILDREN,
                                &node->as.var_decl.type, item.curr_func};
        break;
      case AST_BINOP: {
        DataType *operand_expected = item.expected;

        if (node->as.binop.op.type == TOKEN_COMPARE) {
          operand_expected = NULL;
        }

        stack[top++] = (TCItem){node->as.binop.right, TC_VISIT_CHILDREN,
                                operand_expected, item.curr_func};
        stack[top++] = (TCItem){node->as.binop.left, TC_VISIT_CHILDREN,
                                operand_expected, item.curr_func};
        break;
      }
      case AST_UOP:
      case AST_ADDR_OF:
      case AST_DEREF: {
        DataType *inner = NULL;
        if (item.expected) {
          inner = arena_alloc(arena, sizeof(DataType));
          *inner = *item.expected;
          inner->ptr_depth++;
        }
        stack[top++] = (TCItem){node->as.unop.operand, TC_VISIT_CHILDREN, inner,
                                item.curr_func};
        break;
      }
      case AST_IF:
        if (node->as.if_check.elseAct)
          stack[top++] = (TCItem){node->as.if_check.elseAct, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        if (node->as.if_check.action)
          stack[top++] = (TCItem){node->as.if_check.action, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        stack[top++] = (TCItem){node->as.if_check.check, TC_VISIT_CHILDREN,
                                &EXPECT_BOOL, item.curr_func};
        break;
      case AST_WHILE:
        if (node->as.while_loop.action)
          stack[top++] = (TCItem){node->as.while_loop.action, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        stack[top++] = (TCItem){node->as.while_loop.check, TC_VISIT_CHILDREN,
                                &EXPECT_BOOL, item.curr_func};
        break;
      case AST_FOR:
        if (node->as.for_loop.action)
          stack[top++] = (TCItem){node->as.for_loop.action, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        if (node->as.for_loop.inc)
          stack[top++] = (TCItem){node->as.for_loop.inc, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        stack[top++] = (TCItem){node->as.for_loop.check, TC_VISIT_CHILDREN,
                                &EXPECT_BOOL, item.curr_func};
        if (node->as.for_loop.init)
          stack[top++] = (TCItem){node->as.for_loop.init, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        break;
      case AST_FUNC_CALL: {
        AstNode *fn_decl = NULL;
        if (node->as.func_call.caller->type == AST_IDENTIF &&
            node->as.func_call.caller->as.identif.res_sm) {
          fn_decl = node->as.func_call.caller->as.identif.res_sm->decl_node;
        }

        int arg_count = 0;
        AstNode *temp = node->as.func_call.args;
        while (temp) {
          arg_count++;
          temp = temp->next;
        }

        if (arg_count > 0) {
          AstNode **args = malloc(sizeof(AstNode *) * arg_count);
          AstNode **params = malloc(sizeof(AstNode *) * arg_count);

          AstNode *curr_arg = node->as.func_call.args;
          AstNode *curr_param = NULL;
          if (fn_decl && node->as.func_call.caller->type == AST_IDENTIF &&
              node->as.func_call.caller->as.identif.res_sm->kind == SYM_FUNC) {
            curr_param = fn_decl->as.func_def.params;
          }

          for (int i = 0; i < arg_count; i++) {
            args[i] = curr_arg;
            params[i] = curr_param;
            curr_arg = curr_arg->next;
            if (curr_param)
              curr_param = curr_param->next;
          }

          for (int i = arg_count - 1; i >= 0; i--) {
            DataType *p_type = params[i] ? &params[i]->as.fn_param.type : NULL;
            stack[top++] =
                (TCItem){args[i], TC_VISIT_CHILDREN, p_type, item.curr_func};
          }
          free(args);
          free(params);
        }
        stack[top++] = (TCItem){node->as.func_call.caller, TC_VISIT_CHILDREN,
                                NULL, item.curr_func};
        break;
      }
      case AST_CAST:
        if (node->as.cast.op)
          stack[top++] = (TCItem){node->as.cast.op, TC_VISIT_CHILDREN,
                                  &node->as.cast.target, item.curr_func};
        break;
      case AST_RET:
        if (item.curr_func && node->as.ret_stmt.expr) {
          stack[top++] =
              (TCItem){node->as.ret_stmt.expr, TC_VISIT_CHILDREN,
                       &item.curr_func->as.func_def.ret_type, item.curr_func};
        }
        break;
      case AST_INDEX:
        if (node->as.index.index)
          stack[top++] = (TCItem){node->as.index.index, TC_VISIT_CHILDREN, NULL,
                                  item.curr_func};
        if (node->as.index.base)
          stack[top++] = (TCItem){node->as.index.base, TC_VISIT_CHILDREN, NULL,
                                  item.curr_func};
        break;

      case AST_MEMBER:
        if (node->as.member.base)
          stack[top++] = (TCItem){node->as.member.base, TC_VISIT_CHILDREN, NULL,
                                  item.curr_func};
        break;

      case AST_ARRAY_LIT: {
        AstNode *curr = node->as.array_lit.elements;
        while (curr) {
          stack[top++] =
              (TCItem){curr, TC_VISIT_CHILDREN, item.expected, item.curr_func};
          curr = curr->next;
        }
        break;
      }

      case AST_SWITCH:
        if (node->as.switch_stmt.default_case)
          stack[top++] = (TCItem){node->as.switch_stmt.default_case,
                                  TC_VISIT_CHILDREN, NULL, item.curr_func};
        if (node->as.switch_stmt.cases) {
          AstNode *c = node->as.switch_stmt.cases;
          while (c) {
            stack[top++] = (TCItem){c, TC_VISIT_CHILDREN, NULL, item.curr_func};
            c = c->next;
          }
        }
        if (node->as.switch_stmt.check)
          stack[top++] = (TCItem){node->as.switch_stmt.check, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        break;

      case AST_CASE:
        if (node->as.case_stmt.action)
          stack[top++] = (TCItem){node->as.case_stmt.action, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        if (node->as.case_stmt.val)
          stack[top++] = (TCItem){node->as.case_stmt.val, TC_VISIT_CHILDREN,
                                  NULL, item.curr_func};
        break;

      case AST_DEFER:
        if (node->as.defer_stmt.contents)
          stack[top++] = (TCItem){node->as.defer_stmt.contents,
                                  TC_VISIT_CHILDREN, NULL, item.curr_func};
        break;

      case AST_EXTERN: {
        AstNode *curr = node->as.extern_block.contents;
        while (curr) {
          stack[top++] =
              (TCItem){curr, TC_VISIT_CHILDREN, NULL, item.curr_func};
          curr = curr->next;
        }
        break;
      }
      case AST_STRUCT:
      case AST_UNION:
      case AST_ENUM: {
        AstNode *curr =
            (node->type == AST_STRUCT)  ? node->as.struct_def.contents
            : (node->type == AST_UNION) ? node->as.union_def.contents
                                        : node->as.enum_def.contents;
        while (curr) {
          stack[top++] =
              (TCItem){curr, TC_VISIT_CHILDREN, NULL, item.curr_func};
          curr = curr->next;
        }
        break;
      }
      default:
        break;
      }
    } else {
      switch (node->type) {
      case AST_NUM_LIT: {
        if (expected) {
          node->eval_type = *expected;
        } else {
          const char *val_str = node->as.num_lit.val.start;
          int len = node->as.num_lit.val.len;

          bool is_float = false;
          for (int i = 0; i < len; i++) {
            if (val_str[i] == '.') {
              is_float = true;
              break;
            }
          }

          if (is_float) {
            node->eval_type = create_basic_type("f32");
          } else {
            long long val = atoll(val_str);
            if (val >= -128 && val <= 127)
              node->eval_type = create_basic_type("i8");
            else if (val >= -32768 && val <= 32767)
              node->eval_type = create_basic_type("i16");
            else if (val >= -2147483648LL && val <= 2147483647LL)
              node->eval_type = create_basic_type("i32");
            else
              node->eval_type = create_basic_type("i64");
          }
        }
        break;
      }
      case AST_STR_LIT:
        node->eval_type = create_basic_type("str");
        break;
      case AST_BOOL_LIT:
        node->eval_type = create_basic_type("bool");
        break;
      case AST_CHAR_LIT:
        node->eval_type = create_basic_type("char");
        break;
      case AST_NULL_LIT:
        node->eval_type = create_basic_type("null");
        break;
      case AST_IDENTIF:
        if (node->as.identif.res_sm) {
          Sym *sym = node->as.identif.res_sm;
          if (sym->kind == SYM_VAR && sym->decl_node) {
            if (sym->decl_node->type == AST_PARAM)
              node->eval_type = sym->decl_node->as.fn_param.type;
            else
              node->eval_type = sym->decl_node->as.var_decl.type;
          } else if (sym->kind == SYM_FUNC && sym->decl_node) {
            node->eval_type = sym->decl_node->as.func_def.ret_type;
          } else if (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION ||
                     sym->kind == SYM_ENUM) {
            node->eval_type = create_basic_type("");
            node->eval_type.name = sym->name;
            node->eval_type.is_custom = true;
          }
        }
        break;
      case AST_VAR_DECL:
        if (node->as.var_decl.init) {
          if (!is_type_compatible(node->as.var_decl.type,
                                  node->as.var_decl.init->eval_type, false,
                                  ctx)) {
            sem_report(ctx, DIAG_ERROR, node->as.var_decl.id,
                       "Incompatible assignment for variable '%.*s'",
                       node->as.var_decl.id.len, node->as.var_decl.id.start);
          }
        }
        break;
      case AST_BINOP: {
        DataType left_t = node->as.binop.left->eval_type;
        DataType right_t = node->as.binop.right->eval_type;

        if (node->as.binop.op.start[0] == '/') {
          AstNode *right = node->as.binop.right;

          if (right->type == AST_NUM_LIT) {
            if (atof(right->as.num_lit.val.start) == 0.0) {
              sem_report(ctx, DIAG_ERROR, right->as.num_lit.val,
                         "Error at line %u, col %u: Division by zero.\n",
                         node->as.binop.op.line, node->as.binop.op.col);
            }
          }
        }

        if (!is_type_compatible(left_t, right_t, false, ctx) &&
            !is_type_compatible(right_t, left_t, false, ctx)) {
          sem_report(ctx, DIAG_ERROR, node->as.binop.op,
                     "Type Error at %u:%u: Incompatible operands '%.*s' and "
                     "'%.*s' for "
                     "'%.*s'.\n",
                     node->as.binop.op.line, node->as.binop.op.col,
                     left_t.name.len, left_t.name.start, right_t.name.len,
                     right_t.name.start, node->as.binop.op.len,
                     node->as.binop.op.start);
        }

        if (node->as.binop.op.type == TOKEN_COMPARE) {
          node->eval_type = EXPECT_BOOL;
        } else if (item.expected) {
          node->eval_type = *item.expected;
        } else {
          int l_w = 0, r_w = 0;
          bool l_s, r_s, l_f, r_f;
          get_numeric_info(left_t, &l_w, &l_s, &l_f);
          get_numeric_info(right_t, &r_w, &r_s, &r_f);

          node->eval_type = (l_w >= r_w) ? left_t : right_t;
        }
        break;
      }
      case AST_CAST:
        if (!is_type_compatible(node->as.cast.target,
                                node->as.cast.op->eval_type, true, ctx)) {
          sem_report(ctx, DIAG_ERROR, node->as.cast.target.name,
                     "Type Error at %u:%u: Invalid explicit cast.\n",
                     node->as.cast.target.name.line,
                     node->as.cast.target.name.col);
        }
        node->eval_type = node->as.cast.target;
        break;
      case AST_RET:
        if (item.curr_func && node->as.ret_stmt.expr) {
          if (!is_type_compatible(item.curr_func->as.func_def.ret_type,
                                  node->as.ret_stmt.expr->eval_type, false,
                                  ctx)) {
            sem_report(ctx, DIAG_ERROR, node->as.ret_stmt.expr->eval_type.name,
                       "Type Error at %u:%u: Function return type mismatch.\n",
                       node->as.ret_stmt.ret_kw.line,
                       node->as.ret_stmt.ret_kw.col);
          }
        }
        break;
      case AST_FUNC_CALL:
        if (node->as.func_call.caller->type == AST_IDENTIF &&
            node->as.func_call.caller->as.identif.res_sm &&
            node->as.func_call.caller->as.identif.res_sm->decl_node) {

          Sym *sym = node->as.func_call.caller->as.identif.res_sm;

          if (sym->kind == SYM_FUNC) {
            node->eval_type = sym->decl_node->as.func_def.ret_type;
          } else if (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION) {
            node->eval_type.name = sym->name;
            node->eval_type.is_custom = true;
          } else {
            node->eval_type = create_basic_type("any");
          }
        } else if (node->as.func_call.caller->type == AST_MEMBER) {
          node->eval_type = create_basic_type("any");
        }
        break;
      case AST_ARRAY_LIT:
        if (item.expected) {
          node->eval_type = *item.expected;
        } else if (node->as.array_lit.elements) {
          node->eval_type = node->as.array_lit.elements->eval_type;
          node->eval_type.array_dimens++;
        }
        break;
      case AST_BLOCK: {
        AstNode *last = node->as.block.first_stmt;
        while (last && last->next) {
          last = last->next;
        }
        if (last) {
          node->eval_type = last->eval_type;
        }
        break;
      }
      case AST_IF:
        if (node->as.if_check.action) {
          node->eval_type = node->as.if_check.action->eval_type;
        }
        break;

      case AST_INDEX:
        if (node->as.index.base) {
          node->eval_type = node->as.index.base->eval_type;
          if (node->eval_type.array_dimens > 0) {
            node->eval_type.array_dimens--;
          }
        }
        break;

      case AST_ADDR_OF:
        if (node->as.unop.operand) {
          node->eval_type = node->as.unop.operand->eval_type;
          node->eval_type.ptr_depth++;
        }
        break;

      case AST_DEREF:
        if (node->as.unop.operand) {
          node->eval_type = node->as.unop.operand->eval_type;
          node->eval_type.ptr_depth--;
        }
        break;

      case AST_UOP:
        if (node->as.unop.operand) {
          node->eval_type = node->as.unop.operand->eval_type;
        }
        break;
      case AST_MEMBER: {
        if (node->as.member.base) {
          DataType base_t = node->as.member.base->eval_type;
          if (base_t.name.len > 0) {
            node->as.member.type = base_t.name;
          }
        }
        node->eval_type = create_basic_type("any");
        break;
      }
      default:
        break;
      }
    }
  }
  free(stack);
}
