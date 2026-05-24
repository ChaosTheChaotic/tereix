#include "ast_types.h"
#include "c_gen_types.h"
#include "lsp.h"
#include "sem_types.h"
#include "util.h"
#include "worklist.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void compile_project(const char *entry_file) {
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

    printf("Compiling %s\n", abs_path);
    AstNode *ast = file_to_ast(&arena, abs_path, false);
    if (!ast) {
      fprintf(stderr, "No ast found after trying to parse %s\n", abs_path);
      exit(1);
    }

    const char *mod_name = extract_mod_name(&arena, abs_path);
    Module *mod = new_mod(&arena, abs_path, mod_name, ast);

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
          printf("Compiled successfully");
        else
          fprintf(stderr, "Failed to compile %s", entry_file);
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
