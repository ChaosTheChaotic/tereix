#include "ast_serde.h"
#include "c_gen_types.h"
#include "hashutils.h"
#include "lsp.h"
#include "util.h"
#include "worklist.h"
#include <sys/stat.h>

extern Module *sem_current_mod;
extern Module *sem_main_mod;

bool check_exists(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "r")) != NULL) {
    fclose(fp);
    return true;
  } else {
    return false;
  }
}
inline void print_help() {
  printf("Literally just give it a valid file bro smh");
}

void ensure_cache_dir() {
  struct stat st = {0};
  if (stat(".tx_cache", &st) == -1) {
#if defined(_WIN32)
    mkdir(".tx_cache");
#else
    mkdir(".tx_cache", 0700);
#endif
  }
}

void print_type_info(DataType type) {
  if (type.ptr_depth != 0) {
    char symbol = (type.ptr_depth < 0) ? '*' : '&';
    int count = (type.ptr_depth > 0) ? type.ptr_depth : -type.ptr_depth;

    for (int i = 0; i < count; i++) {
      putchar(symbol);
    }
    putchar(' ');
  }

  if (type.is_static)
    printf("static ");
  if (type.is_mut)
    printf("mut ");
  if (type.is_threadlocal)
    printf("threadlocal ");
  if (type.is_extern)
    printf("extern ");

  printf("%.*s", type.name.len, type.name.start);

  for (unsigned int i = 0; i < type.array_dimens; i++) {
    if (type.dim_sizes != NULL && type.dim_sizes[i] != NULL) {
      AstNode *dim_node = type.dim_sizes[i];

      if (dim_node->type == AST_NUM_LIT) {
        printf("[%.*s]", dim_node->as.num_lit.val.len,
               dim_node->as.num_lit.val.start);
      } else {
        printf("[expr]");
      }
    } else {
      printf("[]");
    }
  }
}

void print_ast(AstNode *root) {
  if (!root)
    return;

  size_t stack_cap = 1024;
  AstPrintItem *stack = malloc(sizeof(AstPrintItem) * stack_cap);
  size_t top = 0;

  stack[top++] = (AstPrintItem){root, 0, "Root"};

  while (top > 0) {
    AstPrintItem item = stack[--top];
    AstNode *node = item.node;

    if (!node)
      continue;

    for (int i = 0; i < item.depth; i++) {
      printf("  | ");
    }

    printf("[%s] ", item.label);

    if (node->next) {
      if (top >= stack_cap - 5) {
        stack_cap *= 2;
        stack = realloc(stack, sizeof(AstPrintItem) * stack_cap);
      }
      stack[top++] = (AstPrintItem){node->next, item.depth, item.label};
    }

    int next_depth = item.depth + 1;

    switch (node->type) {
    case AST_PROGRAM:
      printf("PROGRAM\n");
      if (node->as.block.first_stmt) {
        stack[top++] =
            (AstPrintItem){node->as.block.first_stmt, next_depth, "Decl"};
      }
      break;

    case AST_FUNC:
      if (node->as.func_def.is_extern) {
        printf("EXTERN ");
      }
      if (node->as.func_def.is_async) {
        printf("ASYNC ");
      }
      if (node->as.func_def.is_inline) {
        printf("INLINE ");
      }
      printf("FUNC (Return: ");
      print_type_info(node->as.func_def.ret_type);
      printf("): %.*s\n", node->as.func_def.fn_name.len,
             node->as.func_def.fn_name.start);

      if (node->as.func_def.block) {
        stack[top++] =
            (AstPrintItem){node->as.func_def.block, next_depth, "Body"};
      } else {
        for (int i = 0; i < next_depth; i++)
          printf("  | ");
        printf("[Prototype]\n");
      }
      if (node->as.func_def.params) {
        stack[top++] =
            (AstPrintItem){node->as.func_def.params, next_depth, "Param"};
      }
      break;

    case AST_PARAM:
      printf("PARAM (");
      print_type_info(node->as.fn_param.type);
      printf("): %.*s\n", node->as.fn_param.id.len, node->as.fn_param.id.start);
      break;

    case AST_VAR_DECL:
      printf("VAR_DECL (");
      print_type_info(node->as.var_decl.type);
      printf("): %.*s\n", node->as.var_decl.id.len, node->as.var_decl.id.start);

      if (node->as.var_decl.init) {
        stack[top++] =
            (AstPrintItem){node->as.var_decl.init, next_depth, "Init"};
      }
      break;

    case AST_BINOP:
      printf("BINOP (%.*s)\n", node->as.binop.op.len, node->as.binop.op.start);
      stack[top++] = (AstPrintItem){node->as.binop.right, next_depth, "Right"};
      stack[top++] = (AstPrintItem){node->as.binop.left, next_depth, "Left"};
      break;

    case AST_UOP:
      if (node->as.unop.is_postfix) {
        printf("POSTFIX_UOP (%.*s)\n", node->as.unop.op.len,
               node->as.unop.op.start);
      } else {
        printf("PREFIX_UOP (%.*s)\n", node->as.unop.op.len,
               node->as.unop.op.start);
      }
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    case AST_NUM_LIT:
      printf("NUM_LIT: %.*s\n", node->as.num_lit.val.len,
             node->as.num_lit.val.start);
      break;

    case AST_CHAR_LIT:
      printf("CHAR_LIT: %.*s\n", node->as.char_lit.val.len,
             node->as.char_lit.val.start);
      break;

    case AST_IDENTIF:
      printf("IDENTIF: %.*s\n", node->as.identif.val.len,
             node->as.identif.val.start);
      break;

    case AST_BLOCK:
      if (node->as.block.is_async) {
        printf("ASYNC ");
      }
      printf("BLOCK\n");
      if (node->as.block.first_stmt) {
        stack[top++] =
            (AstPrintItem){node->as.block.first_stmt, next_depth, "Stmt"};
      }
      break;

    case AST_IF:
      printf("IF_STMT\n");
      if (node->as.if_check.elseAct)
        stack[top++] =
            (AstPrintItem){node->as.if_check.elseAct, next_depth, "Else"};
      if (node->as.if_check.action)
        stack[top++] =
            (AstPrintItem){node->as.if_check.action, next_depth, "Then"};
      if (node->as.if_check.check)
        stack[top++] =
            (AstPrintItem){node->as.if_check.check, next_depth, "Cond"};
      break;

    case AST_RET:
      printf("RETURN\n");
      if (node->as.ret_stmt.expr)
        stack[top++] =
            (AstPrintItem){node->as.ret_stmt.expr, next_depth, "Expr"};
      break;

    case AST_FOR:
      printf("FOR_LOOP\n");
      if (node->as.for_loop.action)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.action, next_depth, "Body"};
      if (node->as.for_loop.inc)
        stack[top++] = (AstPrintItem){node->as.for_loop.inc, next_depth, "Inc"};
      if (node->as.for_loop.check)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.check, next_depth, "Cond"};
      if (node->as.for_loop.init)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.init, next_depth, "Init"};
      break;

    case AST_FUNC_CALL:
      printf("FUNC_CALL\n");
      if (node->as.func_call.args)
        stack[top++] =
            (AstPrintItem){node->as.func_call.args, next_depth, "Args"};
      if (node->as.func_call.caller)
        stack[top++] =
            (AstPrintItem){node->as.func_call.caller, next_depth, "Caller"};
      break;

    case AST_WHILE:
      printf("WHILE_LOOP\n");
      if (node->as.while_loop.action)
        stack[top++] =
            (AstPrintItem){node->as.while_loop.action, next_depth, "Body"};
      if (node->as.while_loop.check)
        stack[top++] =
            (AstPrintItem){node->as.while_loop.check, next_depth, "Cond"};
      break;

    case AST_BOOL_LIT:
      printf("BOOL_LIT: %.*s\n", node->as.bool_lit.val.len,
             node->as.bool_lit.val.start);
      break;

    case AST_STR_LIT:
      printf("STR_LIT: %.*s\n", node->as.str_lit.val.len,
             node->as.str_lit.val.start);
      break;

    case AST_ENUM:
      printf("ENUM: %.*s\n", node->as.enum_def.enumn.len,
             node->as.enum_def.enumn.start);
      if (node->as.enum_def.contents) {
        stack[top++] =
            (AstPrintItem){node->as.enum_def.contents, next_depth, "Members"};
      }
      break;

    case AST_ENUM_MEMBER:
      printf("ENUM_MEMBER: %.*s\n", node->as.enum_member.name.len,
             node->as.enum_member.name.start);
      if (node->as.enum_member.val) {
        stack[top++] =
            (AstPrintItem){node->as.enum_member.val, next_depth, "Value"};
      }
      break;

    case AST_ARRAY_LIT:
      printf("ARRAY_LIT\n");
      if (node->as.array_lit.elements) {
        stack[top++] =
            (AstPrintItem){node->as.array_lit.elements, next_depth, "Elem"};
      }
      break;

    case AST_INDEX:
      printf("INDEX_ACCESS\n");
      stack[top++] = (AstPrintItem){node->as.index.index, next_depth, "Index"};
      stack[top++] = (AstPrintItem){node->as.index.base, next_depth, "Base"};
      break;

    case AST_MEMBER:
      printf("MEMBER_ACCESS: .%.*s\n", node->as.member.name.len,
             node->as.member.name.start);
      stack[top++] = (AstPrintItem){node->as.member.base, next_depth, "Base"};
      break;

    case AST_DEFER:
      printf("DEFER\n");
      if (node->as.defer_stmt.contents)
        stack[top++] =
            (AstPrintItem){node->as.defer_stmt.contents, next_depth, "Action"};
      break;

    case AST_STRUCT:
      printf("STRUCT: %.*s\n", node->as.struct_def.structn.len,
             node->as.struct_def.structn.start);
      if (node->as.struct_def.contents)
        stack[top++] =
            (AstPrintItem){node->as.struct_def.contents, next_depth, "Fields"};
      break;

    case AST_UNION:
      printf("UNION: %.*s\n", node->as.union_def.unionn.len,
             node->as.union_def.unionn.start);
      if (node->as.union_def.contents)
        stack[top++] =
            (AstPrintItem){node->as.union_def.contents, next_depth, "Fields"};
      break;

    case AST_ADDR_OF:
      printf("ADDRESS_OF\n");
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    case AST_DEREF:
      printf("DEREF\n");
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    case AST_EXTERN:
      printf("EXTERN_BLOCK\n");
      if (node->as.extern_block.contents)
        stack[top++] = (AstPrintItem){node->as.extern_block.contents,
                                      next_depth, "Contents"};
      break;

    case AST_SWITCH:
      printf("SWITCH\n");
      if (node->as.switch_stmt.default_case)
        stack[top++] = (AstPrintItem){node->as.switch_stmt.default_case,
                                      next_depth, "Default"};
      if (node->as.switch_stmt.cases)
        stack[top++] =
            (AstPrintItem){node->as.switch_stmt.cases, next_depth, "Cases"};
      if (node->as.switch_stmt.check)
        stack[top++] =
            (AstPrintItem){node->as.switch_stmt.check, next_depth, "Cond"};
      break;

    case AST_CASE:
      printf("CASE\n");
      if (node->as.case_stmt.action)
        stack[top++] =
            (AstPrintItem){node->as.case_stmt.action, next_depth, "Action"};
      if (node->as.case_stmt.val)
        stack[top++] =
            (AstPrintItem){node->as.case_stmt.val, next_depth, "Value"};
      break;

    case AST_USE:
      printf("USE: %.*s", node->as.use_stmt.path.len,
             node->as.use_stmt.path.start);
      if (node->as.use_stmt.alias.len > 0) {
        printf(" as %.*s", node->as.use_stmt.alias.len,
               node->as.use_stmt.alias.start);
      }
      printf("\n");
      break;

    case AST_NULL_LIT:
      printf("NULL_LIT\n");
      break;

    case AST_BREAK:
      printf("BREAK\n");
      break;

    case AST_CONTINUE:
      printf("CONTINUE\n");
      break;

    case AST_CAST:
      printf("CAST TO (");
      print_type_info(node->as.cast.target);
      printf(")\n");
      stack[top++] = (AstPrintItem){node->as.cast.op, next_depth, "Operand"};
      break;

    default:
      printf("AST_NODE_TYPE_%d\n", node->type);
      break;
    }
  }

  free(stack);
  printf("\n");
}

void extract_dependencies(AstNode *root, void (*callback)(Token callee_name)) {
  if (!root)
    return;

  size_t stack_cap = 1024;
  AstNode **stack = malloc(sizeof(AstNode *) * stack_cap);
  size_t top = 0;

  stack[top++] = root;

  while (top > 0) {
    AstNode *node = stack[--top];
    if (!node)
      continue;

    // Check for explicit Function Calls or Method calls
    if (node->type == AST_FUNC_CALL && node->as.func_call.caller) {
      if (node->as.func_call.caller->type == AST_IDENTIF) {
        callback(node->as.func_call.caller->as.identif.val);
      } else if (node->as.func_call.caller->type == AST_MEMBER) {
        callback(node->as.func_call.caller->as.member.name);
      }
    }

    // Push sibling to stack first
    if (node->next) {
      if (top >= stack_cap - 2) {
        stack_cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * stack_cap);
      }
      stack[top++] = node->next;
    }

#define PUSH_NODE(n)                                                           \
  do {                                                                         \
    if (n) {                                                                   \
      if (top >= stack_cap - 2) {                                              \
        stack_cap *= 2;                                                        \
        stack = realloc(stack, sizeof(AstNode *) * stack_cap);                 \
      }                                                                        \
      stack[top++] = (n);                                                      \
    }                                                                          \
  } while (0)

    // Queue up the children for subsequent loops
    switch (node->type) {
    case AST_BINOP:
      PUSH_NODE(node->as.binop.left);
      PUSH_NODE(node->as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PUSH_NODE(node->as.unop.operand);
      break;
    case AST_IF:
      PUSH_NODE(node->as.if_check.check);
      PUSH_NODE(node->as.if_check.action);
      PUSH_NODE(node->as.if_check.elseAct);
      break;
    case AST_BLOCK:
    case AST_PROGRAM:
      PUSH_NODE(node->as.block.first_stmt);
      break;
    case AST_FUNC:
      PUSH_NODE(node->as.func_def.block);
      break;
    case AST_RET:
      PUSH_NODE(node->as.ret_stmt.expr);
      break;
    case AST_VAR_DECL:
      PUSH_NODE(node->as.var_decl.init);
      break;
    case AST_ARRAY_LIT:
      PUSH_NODE(node->as.array_lit.elements);
      break;
    case AST_STRUCT:
      PUSH_NODE(node->as.struct_def.contents);
      break;
    case AST_UNION:
      PUSH_NODE(node->as.union_def.contents);
      break;
    case AST_ENUM:
      PUSH_NODE(node->as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      PUSH_NODE(node->as.enum_member.val);
      break;
    case AST_DEFER:
      PUSH_NODE(node->as.defer_stmt.contents);
      break;
    case AST_FOR:
      PUSH_NODE(node->as.for_loop.init);
      PUSH_NODE(node->as.for_loop.check);
      PUSH_NODE(node->as.for_loop.inc);
      PUSH_NODE(node->as.for_loop.action);
      break;
    case AST_WHILE:
      PUSH_NODE(node->as.while_loop.check);
      PUSH_NODE(node->as.while_loop.action);
      break;
    case AST_FUNC_CALL:
      PUSH_NODE(node->as.func_call.caller);
      PUSH_NODE(node->as.func_call.args);
      break;
    case AST_INDEX:
      PUSH_NODE(node->as.index.base);
      PUSH_NODE(node->as.index.index);
      break;
    case AST_MEMBER:
      PUSH_NODE(node->as.member.base);
      break;
    case AST_SWITCH:
      PUSH_NODE(node->as.switch_stmt.check);
      PUSH_NODE(node->as.switch_stmt.cases);
      PUSH_NODE(node->as.switch_stmt.default_case);
      break;
    case AST_CASE:
      PUSH_NODE(node->as.case_stmt.val);
      PUSH_NODE(node->as.case_stmt.action);
      break;
    case AST_EXTERN:
      PUSH_NODE(node->as.extern_block.contents);
      break;
    case AST_CAST:
      PUSH_NODE(node->as.cast.op);
      break;
    case AST_SIZEOF:
      PUSH_NODE(node->as.sizeof_expr.target_expr);
      break;
    default:
      break;
    }

#undef PUSH_NODE
  }

  free(stack);
}

void sync_dirty_flags_to_ast(Module *mod) {
  if (!mod->ast_root || mod->ast_root->type != AST_PROGRAM)
    return;

  AstNode *stmt = mod->ast_root->as.block.first_stmt;
  while (stmt) {
    Token name = {0};

    if (stmt->type == AST_FUNC)
      name = stmt->as.func_def.fn_name;
    else if (stmt->type == AST_STRUCT)
      name = stmt->as.struct_def.structn;
    else if (stmt->type == AST_UNION)
      name = stmt->as.union_def.unionn;
    else if (stmt->type == AST_ENUM)
      name = stmt->as.enum_def.enumn;
    else if (stmt->type == AST_VAR_DECL)
      name = stmt->as.var_decl.id;

    if (name.start) {
      stmt->is_dirty = false;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        // Match AST top level nodes back to their cached metadata
        if (m->name.len == name.len &&
            memcmp(m->name.start, name.start, name.len) == 0) {
          stmt->is_dirty = m->is_dirty;
          break;
        }
      }
    } else {
      stmt->is_dirty = true;
    }
    stmt = stmt->next;
  }
}

typedef struct DepList {
  DeclMetadata *decl;
  struct DepList *next;
} DepList;

void propagate_global_invalidation(SemCtx *sem) {
  size_t total_decls = 0;
  Arena temp_arena = {0}; // Temporary arena for the reverse graph
  HashMap reverse_deps;
  map_init(&reverse_deps, &temp_arena, 4096);

  // Build the Reverse Dependency Graph
  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        total_decls++;

        for (DepNode *dep = m->calls_to; dep; dep = dep->next) {
          DepList *link = arena_alloc(&temp_arena, sizeof(DepList));
          link->decl = m;
          link->next = map_get(&reverse_deps, dep->name.start, dep->name.len);
          map_set(&reverse_deps, dep->name.start, dep->name.len, link);
        }

        for (DepNode *dep = m->uses_types; dep; dep = dep->next) {
          DepList *link = arena_alloc(&temp_arena, sizeof(DepList));
          link->decl = m;
          link->next = map_get(&reverse_deps, dep->name.start, dep->name.len);
          map_set(&reverse_deps, dep->name.start, dep->name.len, link);
        }
      }
      entry = entry->next;
    }
  }

  if (total_decls == 0)
    return;

  DeclMetadata **worklist = malloc(sizeof(DeclMetadata *) * total_decls);
  size_t wl_top = 0;

  // Queue initially dirty items
  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        if (m->is_dirty)
          worklist[wl_top++] = m;
      }
      entry = entry->next;
    }
  }

  // Propagate using the reverse graph
  while (wl_top > 0) {
    DeclMetadata *dirty_item = worklist[--wl_top];

    // Instantly get all declarations that depend on this dirty item
    DepList *dependents =
        map_get(&reverse_deps, dirty_item->name.start, dirty_item->name.len);

    while (dependents) {
      DeclMetadata *dependent_decl = dependents->decl;
      if (!dependent_decl->is_dirty) {
        dependent_decl->is_dirty = true;
        worklist[wl_top++] = dependent_decl;
      }
      dependents = dependents->next;
    }
  }

  free(worklist);
  arena_free_all(&temp_arena);
}

void compile_project(const char *entry_file) {
  ensure_cache_dir();
  Arena arena = {0};
  SemCtx sem = {0};
  sem_init(&sem, &arena);

  // Initialize module mapping tracking configurations
  sem_current_mod = NULL;
  sem_main_mod = NULL;

  Worklist pending = {0};
  wl_push(&pending, entry_file);

  const char *current_path;
  while ((current_path = wl_pop(&pending)) != NULL) {
    const char *abs_path = resolve_alloc(&arena, current_path);
    if (!abs_path || map_get(&sem.mod_cache, abs_path, strlen(abs_path)))
      continue;

    const char *content = load_file(abs_path);
    uint64_t curr_hash = hash_string(content, strlen(content));
    uint64_t path_hash = hash_string(abs_path, strlen(abs_path));

    char cache_file[512];
    char meta_file[512];
    snprintf(cache_file, sizeof(cache_file), ".tx_cache/%lu.cache", path_hash);
    snprintf(meta_file, sizeof(meta_file), ".tx_cache/%lu.meta", path_hash);

    AstNode *ast = NULL;
    bool cache_loaded = false;
    DeclMetadata *meta = NULL;

    // Read the file level content hash
    FILE *fp = fopen(cache_file, "rb");
    uint64_t cached_content_hash = 0;
    if (fp) {
      if (fread(&cached_content_hash, sizeof(uint64_t), 1, fp) != 1) {
        cached_content_hash = 0;
      }
      fclose(fp);
    }

    if (cached_content_hash == curr_hash) {
      printf("Cache hit for %s\n", abs_path);
      // Pass an offset to skip the header hash (8 bytes)
      ast = cache_read_ast(&arena, cache_file, content);
      if (ast) {
        cache_loaded = true;
        // Load clean metadata since the file didn't change
        meta = cache_read_decl_meta(&arena, meta_file, content);
        if (meta) {
          for (DeclMetadata *m = meta; m; m = m->next) {
            m->is_dirty = false;
          }
        }
      }
    }

    if (!cache_loaded) {
      printf("Compiling %s\n", abs_path);

      DiagList diags;
      diaglist_init(&diags, 1024);

      ast = str_to_ast(&arena, content, abs_path, &diags, false);

      if (!ast) {
        for (size_t i = 0; i < diags.count; i++) {
          printf("Error on %u:%u in file %s: %s\n", diags.items[i].start_line,
                 diags.items[i].start_char, diags.items[i].file,
                 diags.items[i].message);
        }
        fprintf(stderr, "No AST found after trying to parse %s\n", abs_path);
        exit(1);
      }
      diaglist_free(&diags);

      meta = analyze_module_declarations(&arena, ast);
      DeclMetadata *old_meta = cache_read_decl_meta(&arena, meta_file, content);

      propagate_declaration_invalidation(old_meta, meta);

      // Early dirty flag sync so we know what is safe to splice
      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        Token name = {0};
        if (stmt->type == AST_FUNC)
          name = stmt->as.func_def.fn_name;
        else if (stmt->type == AST_STRUCT)
          name = stmt->as.struct_def.structn;
        else if (stmt->type == AST_UNION)
          name = stmt->as.union_def.unionn;
        else if (stmt->type == AST_ENUM)
          name = stmt->as.enum_def.enumn;
        else if (stmt->type == AST_VAR_DECL)
          name = stmt->as.var_decl.id;

        stmt->is_dirty = true;
        if (name.start) {
          for (DeclMetadata *m = meta; m; m = m->next) {
            if (m->name.len == name.len &&
                memcmp(m->name.start, name.start, name.len) == 0) {
              stmt->is_dirty = m->is_dirty;
              break;
            }
          }
        }
        stmt = stmt->next;
      }

// Swap fully typed cached nodes into the fresh AST
      AstNode *old_ast = cache_read_ast(&arena, cache_file, content);
      AstNode **ptr = &ast->as.block.first_stmt;

      while (*ptr) {
        AstNode *curr = *ptr;
        if (!curr->is_dirty &&
            (curr->type == AST_FUNC || curr->type == AST_VAR_DECL ||
             curr->type == AST_STRUCT || curr->type == AST_UNION ||
             curr->type == AST_ENUM)) {

          bool spliced = false;
          if (old_ast) {
            AstNode *old_stmt = old_ast->as.block.first_stmt;
            while (old_stmt) {
              if (old_stmt->node_hash == curr->node_hash &&
                  old_stmt->type == curr->type) {
                old_stmt->next = curr->next;
                old_stmt->is_dirty = false;
                *ptr = old_stmt; // Splice it in
                spliced = true;
                break;
              }
              old_stmt = old_stmt->next;
            }
          }

          if (!spliced) {
            curr->is_dirty = true;

            Token name = {0};
            if (curr->type == AST_FUNC)
              name = curr->as.func_def.fn_name;
            else if (curr->type == AST_STRUCT)
              name = curr->as.struct_def.structn;
            else if (curr->type == AST_UNION)
              name = curr->as.union_def.unionn;
            else if (curr->type == AST_ENUM)
              name = curr->as.enum_def.enumn;
            else if (curr->type == AST_VAR_DECL)
              name = curr->as.var_decl.id;

            if (name.start) {
              for (DeclMetadata *m = meta; m; m = m->next) {
                if (m->name.len == name.len &&
                    memcmp(m->name.start, name.start, name.len) == 0) {
                  m->is_dirty = true; // Sync metadata back so downstream knows
                  break;
                }
              }
            }
          }
        }
        ptr = &(*ptr)->next;
      }

      // Write only hash marker for now
      // Final, fully typed AST is saved
      // automatically at the bottom
      FILE *out = fopen(cache_file, "wb");
      if (out) {
        fwrite(&curr_hash, sizeof(uint64_t), 1, out);
        fclose(out);
      }

      cache_write_decl_meta(meta_file, meta, content);
    }

    const char *mod_name = extract_mod_name(&arena, abs_path);
    Module *mod = new_mod(&arena, abs_path, mod_name, ast);
    mod->content_hash = curr_hash;
    mod->meta = meta;
    mod->content = content;
    mod->needs_cache_write = !cache_loaded;

    // Track the entry main module layer context first
    if (sem_main_mod == NULL) {
      sem_main_mod = mod;
    }

    printf("Module: %s\n", mod_name);
    print_ast(ast);

    map_set(&sem.mod_cache, abs_path, strlen(abs_path), mod);

    AstNode *stmt = ast->as.block.first_stmt;
    while (stmt) {
      if (stmt->type == AST_USE) {
        size_t path_len = stmt->as.use_stmt.path.len;
        if (path_len > 2) {
          char *clean_rel = arena_alloc(&arena, path_len - 1);
          strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
          clean_rel[path_len - 2] = '\0';
          wl_push(&pending, clean_rel);
        }
      }
      stmt = stmt->next;
    }
  }

  printf("AST Construction complete.\n");

  // Run the cross-module invalidation engine
  propagate_global_invalidation(&sem);

  bool requires_rebuild = false;

  // Sync the metadata dirty flags directly onto the AST nodes
  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sync_dirty_flags_to_ast(mod);

      AstNode *stmt = mod->ast_root->as.block.first_stmt;
      while (stmt) {
        if (stmt->is_dirty)
          requires_rebuild = true;
        stmt = stmt->next;
      }
      entry = entry->next;
    }
  }

  if (!requires_rebuild) {
    printf("Project is up to date.\n");
    if (pending.paths)
      free((void *)pending.paths);
    sem_deinit(&sem);
    arena_free_all(&arena);
    return;
  }

  printf("Global dependency propagation complete.\n");

  resolve_imports(&arena, &sem);
  printf("Import graph resolved.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sem_current_mod = mod;
      collect_mod_symbols(&arena, mod, &sem);
      entry = entry->next;
    }
  }

  printf("Symbol collection complete.\n");

  ScopeStack ss;
  scope_stack_init(&ss, &arena);

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      ss.count = 0;
      sem_current_mod = mod;
      resolve_scopes(&arena, mod, &ss, &sem);

      entry = entry->next;
    }
  }

  printf("Scope resolution complete.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sem_current_mod = mod;
      type_check_ast(&arena, mod->ast_root, &sem);

      entry = entry->next;
    }
  }

  printf("Type checking complete.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      if (mod->needs_cache_write) {
        char cache_file[512];
        uint64_t path_hash = hash_string(mod->abs_path, strlen(mod->abs_path));
        snprintf(cache_file, sizeof(cache_file), ".tx_cache/%lu.cache",
                 path_hash);

        // Write the hash header, then the fully typed AST
        FILE *out = fopen(cache_file, "wb");
        if (out) {
          fwrite(&mod->content_hash, sizeof(uint64_t), 1, out);
          fclose(out);
          cache_write_ast(cache_file, mod->ast_root, mod->content);
        }
      }
      entry = entry->next;
    }
  }

  const char *abs_path = resolve_alloc(&arena, entry_file);
  Module *main_mod = map_get(&sem.mod_cache, abs_path, strlen(abs_path));

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = entry->value;

      if (strncmp(mod->abs_path, abs_path, strlen(abs_path)) == 0) {
        printf("Transpiling main module: %s\n", entry_file);

        const char *flags[] = {"-O3", "-flto", "-Wno-strict-prototypes",
                               "-Wextra", "-Wpedantic"};

        const char *base = strrchr(entry_file, '/');
        if (base) {
          base++;
        } else {
          base = entry_file;
        }

        char *bin_name = arena_alloc(&arena, strlen(base) + 1);
        strcpy(bin_name, base);
        char *dot = strrchr(bin_name, '.');
        if (dot && strcmp(dot, ".tx") == 0)
          *dot = '\0';

        bool suc =
            output_to_c_and_compile(&sem, bin_name, flags, 5, &arena, main_mod);
        if (suc)
          printf("Compiled successfully\n");
        else
          fprintf(stderr, "Failed to compile %s\n", entry_file);
      }
      break;
    }
    entry = entry->next;
  }

  if (pending.paths)
    free((void *)pending.paths);
  sem_deinit(&sem);
  arena_free_all(&arena);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--lsp") == 0) {
    start_lsp_server();
    return 0;
  }

  if (argc <= 1) {
    print_help();
    exit(1);
  }

  if (check_exists(argv[1])) {
    compile_project(argv[1]);
  } else {
    printf("Entry file does not exist: %s\n", argv[1]);
    return 1;
  }

  return 0;
}
