#include "c_gen_types.h"
#include "lex_types.h"
#include "parse_types.h"
#include "sem_types.h"
#include "string_builder.h"
#include "util.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *load_file(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "rb")) != NULL) {
    if (fseek(fp, 0, SEEK_END) != 0) {
      perror("Error seeking to end of file");
      fclose(fp);
      return NULL;
    }

    long fsize;
    if ((fsize = ftell(fp)) == -1) {
      perror("Failed to get file size");
      fclose(fp);
      return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
      perror("Error seeking to start of file");
      fclose(fp);
      return NULL;
    }

    char *file = malloc(sizeof(char) * (fsize + 1));
    if (!file) {
      fprintf(stderr, "Failed to malloc the file");
      fclose(fp);
      return NULL;
    }

    unsigned long bin = fread(file, sizeof(char), fsize, fp);
    if (bin != (unsigned long)fsize) {
      fprintf(stderr, "Bytes read into buffer != the size of the file");
      free(file);
      fclose(fp);
      return NULL;
    }

    file[fsize] = '\0';
    fclose(fp);
    return file;
  } else {
    return NULL;
  }
}

void init_lex_maps(LexCtx *ctx, Arena *arena) {
  // Capacities padded to powers of 2 for minimal collisions
  map_init(&ctx->kw_map, arena, 64);
  map_init(&ctx->op_map, arena, 64);
  map_init(&ctx->comp_map, arena, 16);
  map_init(&ctx->type_kw_map, arena, 64);

  for (size_t i = 0; i < kwlistlen; i++)
    map_set(&ctx->kw_map, kwlist[i], strlen(kwlist[i]), (void *)1);

  for (size_t i = 0; i < oplistlen; i++)
    map_set(&ctx->op_map, oplist[i], strlen(oplist[i]), (void *)1);

  for (size_t i = 0; i < complistlen; i++)
    map_set(&ctx->comp_map, complist[i], strlen(complist[i]), (void *)1);

  for (size_t i = 0; i < typelistlen; i++)
    map_set(&ctx->type_kw_map, typelist[i], strlen(typelist[i]), (void *)1);
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

    // Print indentation
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

void wl_push(Worklist *wl, const char *path) {
  if (wl->count >= wl->capacity) {
    wl->capacity = (wl->capacity == 0) ? 32 : wl->capacity * 2;
    wl->paths = realloc((void *)wl->paths, sizeof(const char *) * wl->capacity);
  }
  wl->paths[wl->count++] = path;
}

const char *wl_pop(Worklist *wl) {
  if (wl->count == 0)
    return NULL;
  return wl->paths[--wl->count];
}

const char *extract_mod_name(Arena *arena, const char *abs_path) {
  const char *base = strrchr(abs_path, '/');
  base = base ? base + 1 : abs_path;

  const char *ext = strrchr(base, '.');
  size_t len = ext ? (size_t)(ext - base) : strlen(base);

  char *mod_name = arena_alloc(arena, len + 1);
  strncpy(mod_name, base, len);
  mod_name[len] = '\0';

  return mod_name;
}

AstNode *file_to_ast(Arena *arena, const char *path) {
  const char *file = load_file(path);
  if (!file)
    return NULL;

  LexCtx lex = {0};
  lex.start = (char *)file;
  lex.curr = (char *)file;
  lex.line = 1;
  lex.col = 1;
  init_lex_maps(&lex, arena);

  ParseCtx pctx = {0};
  pctx.lex = &lex;
  pctx.arena = arena;
  pctx.curr = next_token(&lex);
  pctx.state_cap = 64;
  pctx.state_stack = malloc(sizeof(ParseState) * pctx.state_cap);

  AstNode *root = new_node(arena, AST_PROGRAM);
  push_node(&pctx, root);

  bool success = parse(&pctx);
  free(pctx.state_stack);

  if (!success) {
    return NULL;
  }
  return root;
}

void gen_type(DataType type, StringBuilder *sb) {
  if (type.is_static)
    sb_append(sb, "static ");
  if (type.is_extern)
    sb_append(sb, "extern ");

  if (!type.is_mut) {
    bool is_void =
        (type.name.len == 4 && strncmp(type.name.start, "void", 4) == 0);
    if (!(is_void && type.ptr_depth == 0 && type.array_dimens == 0)) {
      sb_append(sb, "const ");
    }
  }

  const char *n = type.name.start;
  int l = type.name.len;

  if (l == 3 && strncmp(n, "i32", 3) == 0)
    sb_append(sb, "int32_t");
  else if (l == 3 && strncmp(n, "u32", 3) == 0)
    sb_append(sb, "uint32_t");
  else if (l == 2 && strncmp(n, "i8", 2) == 0)
    sb_append(sb, "int8_t");
  else if (l == 2 && strncmp(n, "u8", 2) == 0)
    sb_append(sb, "uint8_t");
  else if (l == 3 && strncmp(n, "i64", 3) == 0)
    sb_append(sb, "int64_t");
  else if (l == 3 && strncmp(n, "u64", 3) == 0)
    sb_append(sb, "uint64_t");
  else if (l == 3 && strncmp(n, "f32", 3) == 0)
    sb_append(sb, "float");
  else if (l == 3 && strncmp(n, "f64", 3) == 0)
    sb_append(sb, "double");
  else if (l == 4 && strncmp(n, "size", 4) == 0)
    sb_append(sb, "size_t");
  else if (l == 4 && strncmp(n, "bool", 4) == 0)
    sb_append(sb, "bool");
  else if (l == 4 && strncmp(n, "void", 4) == 0)
    sb_append(sb, "void");
  else if (l == 3 && strncmp(n, "str", 3) == 0)
    sb_append(sb, "char*");
  else if (l == 3 && strncmp(n, "any", 3) == 0)
    sb_append(sb, "void*");
  else
    sb_append_len(sb, n, l);

  sb_append(sb, " ");

  // Treat depth and arrays as pointers for C output
  long total_ptrs = type.ptr_depth + type.array_dimens;
  for (long i = 0; i < total_ptrs; i++) {
    sb_append(sb, "*");
  }
}

HashMap *build_func_map(Arena *arena, AstNode *root) {
  HashMap *map = arena_alloc(arena, sizeof(HashMap));
  map_init(map, arena, 64);

  AstNode *stmt = root->as.block.first_stmt;
  while (stmt) {
    if (stmt->type == AST_FUNC) {
      Token name = stmt->as.func_def.fn_name;
      map_set(map, name.start, name.len, stmt);
    }
    stmt = stmt->next;
  }
  return map;
}

void generate_c_code(AstNode *root, StringBuilder *sb, HashMap *func_map,
                     Arena *arena) {
  size_t cap = 2048;
  IterFrame *stack = malloc(sizeof(IterFrame) * cap);
  size_t top = 0;

  stack[top++] = (IterFrame){root, 0, NULL, NULL, 0};

  while (top > 0) {
    IterFrame *f = &stack[top - 1];
    AstNode *n = f->node;

    if (!n) {
      top--;
      continue;
    }

    if (n->type == AST_PROGRAM || n->type == AST_BLOCK ||
        n->type == AST_EXTERN) {
      if (f->step == 0) {
        if (n->type == AST_BLOCK)
          sb_append(sb, "{\n");
        f->aux = (n->type == AST_EXTERN) ? n->as.extern_block.contents
                                         : n->as.block.first_stmt;
        f->step = 1;
      } else if (f->step == 1) {
        if (f->aux) {
          AstNode *stmt = f->aux;
          f->aux = f->aux->next;

          bool needs_semi =
              (stmt->type == AST_FUNC_CALL || stmt->type == AST_BINOP ||
               stmt->type == AST_UOP || stmt->type == AST_IDENTIF ||
               stmt->type == AST_NUM_LIT || stmt->type == AST_STR_LIT ||
               stmt->type == AST_MEMBER);
          f->flags = needs_semi ? 1 : 0;
          f->step = 2;

          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){stmt, 0, NULL, NULL, 0};
        } else {
          f->step = 3;
        }
      } else if (f->step == 2) {
        if (f->flags)
          sb_append(sb, ";\n");
        f->step = 1;
      } else {
        if (n->type == AST_BLOCK)
          sb_append(sb, "}\n");
        top--;
      }
    } else if (n->type == AST_FUNC) {
      if (f->step == 0) {
        if (n->as.func_def.fn_name.len == 4 &&
            strncmp(n->as.func_def.fn_name.start, "main", 4) == 0) {
          AstNode *params = n->as.func_def.params;
          int param_count = 0;
          AstNode *p = params;
          while (p) {
            param_count++;
            p = p->next;
          }

          if (param_count != 2) {
            fprintf(stderr,
                    "Error: main function must have exactly 2 parameters "
                    "(an integer and a string array). Found %d.\n",
                    param_count);
            exit(1);
          }

          AstNode *param1 = params;
          DataType t1 = param1->as.fn_param.type;
          int width;
          bool is_signed, is_float;
          if (!get_numeric_info(t1, &width, &is_signed, &is_float) ||
              width < 32) {
            fprintf(stderr, "Error: first parameter of main must be a integer "
                            "with at least 32 bits (e.g. i32, i64).\n");
            exit(1);
          }

          AstNode *param2 = param1->next;
          DataType t2 = param2->as.fn_param.type;
          if (!(t2.name.len == 3 && strncmp(t2.name.start, "str", 3) == 0 &&
                (t2.ptr_depth + t2.array_dimens >= 1))) {
            fprintf(
                stderr,
                "Error: second parameter of main must be str[] or **str.\n");
            exit(1);
          }

          if (n->as.func_def.is_extern)
            sb_append(sb, "extern ");
          if (n->as.func_def.is_inline)
            sb_append(sb, "inline ");

          sb_append(sb, "int main(");
          sb_append(sb, "int ");
          sb_append_len(sb, param1->as.fn_param.id.start,
                        param1->as.fn_param.id.len);
          sb_append(sb, ", char **");
          sb_append_len(sb, param2->as.fn_param.id.start,
                        param2->as.fn_param.id.len);
          sb_append(sb, ") ");

          f->aux = NULL;
          f->step = 2;

          if (n->as.func_def.block) {
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){n->as.func_def.block, 0, NULL, NULL, 0};
          } else {
            sb_append(sb, ";\n");
            top--;
          }
        } else {
          if (n->as.func_def.is_extern)
            sb_append(sb, "extern ");
          if (n->as.func_def.is_inline)
            sb_append(sb, "inline ");

          DataType clean_ret = n->as.func_def.ret_type;
          clean_ret.is_extern = false;
          clean_ret.is_static = false;
          clean_ret.is_threadlocal = false;
          clean_ret.is_mut = true;

          gen_type(clean_ret, sb);
          sb_append_len(sb, n->as.func_def.fn_name.start,
                        n->as.func_def.fn_name.len);
          sb_append(sb, "(");

          f->aux = n->as.func_def.params;
          f->step = 1;
        }
      } else if (f->step == 1) {
        if (f->aux) {
          AstNode *p = f->aux;
          gen_type(p->as.fn_param.type, sb);
          sb_append_len(sb, p->as.fn_param.id.start, p->as.fn_param.id.len);
          f->aux = f->aux->next;
          if (f->aux)
            sb_append(sb, ", ");
        } else {
          sb_append(sb, ") ");
          if (n->as.func_def.block) {
            f->step = 2;
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){n->as.func_def.block, 0, NULL, NULL, 0};
          } else {
            sb_append(sb, ";\n");
            top--;
          }
        }
      } else {
        sb_append(sb, "\n");
        top--;
      }
    } else if (n->type == AST_VAR_DECL) {
      if (f->step == 0) {
        gen_type(n->as.var_decl.type, sb);
        sb_append_len(sb, n->as.var_decl.id.start, n->as.var_decl.id.len);
        if (n->as.var_decl.init) {
          sb_append(sb, " = ");
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.var_decl.init, 0, NULL, NULL, 0};
        } else {
          sb_append(sb, ";\n");
          top--;
        }
      } else {
        sb_append(sb, ";\n");
        top--;
      }
    } else if (n->type == AST_BINOP) {
      if (f->step == 0) {
        sb_append(sb, "(");
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.binop.left, 0, NULL, NULL, 0};
      } else if (f->step == 1) {
        sb_append(sb, " ");
        sb_append_len(sb, n->as.binop.op.start, n->as.binop.op.len);
        sb_append(sb, " ");
        f->step = 2;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.binop.right, 0, NULL, NULL, 0};
      } else {
        sb_append(sb, ")");
        top--;
      }
    } else if (n->type == AST_UOP || n->type == AST_ADDR_OF ||
               n->type == AST_DEREF) {
      if (f->step == 0) {
        if (n->type == AST_ADDR_OF)
          sb_append(sb, "&(");
        else if (n->type == AST_DEREF)
          sb_append(sb, "*(");
        else {
          if (!n->as.unop.is_postfix)
            sb_append_len(sb, n->as.unop.op.start, n->as.unop.op.len);
          sb_append(sb, "(");
        }
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.unop.operand, 0, NULL, NULL, 0};
      } else {
        sb_append(sb, ")");
        if (n->type == AST_UOP && n->as.unop.is_postfix) {
          sb_append_len(sb, n->as.unop.op.start, n->as.unop.op.len);
        }
        top--;
      }
    } else if (n->type == AST_IF) {
      if (f->step == 0) {
        sb_append(sb, "if (");
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.if_check.check, 0, NULL, NULL, 0};
      } else if (f->step == 1) {
        sb_append(sb, ") ");
        f->step = 2;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.if_check.action, 0, NULL, NULL, 0};
      } else if (f->step == 2) {
        if (n->as.if_check.elseAct) {
          sb_append(sb, " else ");
          f->step = 3;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.if_check.elseAct, 0, NULL, NULL, 0};
        } else {
          top--;
        }
      } else {
        top--;
      }
    } else if (n->type == AST_WHILE) {
      if (f->step == 0) {
        sb_append(sb, "while (");
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.while_loop.check, 0, NULL, NULL, 0};
      } else if (f->step == 1) {
        sb_append(sb, ") ");
        f->step = 2;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.while_loop.action, 0, NULL, NULL, 0};
      } else {
        top--;
      }
    } else if (n->type == AST_FOR) {
      if (f->step == 0) {
        sb_append(sb, "for (");
        f->step = 1;
        if (n->as.for_loop.init) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.for_loop.init, 0, NULL, NULL, 0};
        }
      } else if (f->step == 1) {
        if (!n->as.for_loop.init || n->as.for_loop.init->type != AST_VAR_DECL)
          sb_append(sb, "; ");
        f->step = 2;
        if (n->as.for_loop.check) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.for_loop.check, 0, NULL, NULL, 0};
        }
      } else if (f->step == 2) {
        sb_append(sb, "; ");
        f->step = 3;
        if (n->as.for_loop.inc) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.for_loop.inc, 0, NULL, NULL, 0};
        }
      } else if (f->step == 3) {
        sb_append(sb, ") ");
        f->step = 4;
        if (n->as.for_loop.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.for_loop.action, 0, NULL, NULL, 0};
        }
      } else {
        top--;
      }
    } else if (n->type == AST_FUNC_CALL) {
      if (n->as.func_call.caller &&
          n->as.func_call.caller->type == AST_MEMBER) {
        if (f->step == 0) {
          AstNode *member_node = n->as.func_call.caller;
          AstNode *base = member_node->as.member.base;

          if (base->type == AST_IDENTIF && base->as.identif.res_sm &&
              base->as.identif.res_sm->is_imported_mod) {
            sb_append_len(sb, member_node->as.member.name.start,
                          member_node->as.member.name.len);
            sb_append(sb, "(");
            f->aux = n->as.func_call.args;
            f->aux2 = NULL;
            f->flags = 2;
            f->step = 2;
          } else {
            Token method = member_node->as.member.name;
            DataType base_eval = base->eval_type;
            Token base_type = base_eval.name;
            if (base_type.len == 0) {
              base_type = member_node->as.member.type;
            }

            // Build mangled name
            size_t mangled_len = base_type.len + 1 + method.len;
            char *mangled = arena_alloc(arena, mangled_len + 1);
            memcpy(mangled, base_type.start, base_type.len);
            mangled[base_type.len] = '_';
            memcpy(mangled + base_type.len + 1, method.start, method.len);
            mangled[mangled_len] = '\0';

            // Lookup function to check for self parameter
            AstNode *func_def = map_get(func_map, mangled, mangled_len);
            bool has_self = false;
            if (func_def && func_def->type == AST_FUNC) {
              AstNode *first_param = func_def->as.func_def.params;
              if (first_param) {
                DataType pt = first_param->as.fn_param.type;
                if (pt.name.len == base_type.len &&
                    strncmp(pt.name.start, base_type.start, pt.name.len) == 0 &&
                    pt.ptr_depth == 0 && pt.array_dimens == 0) {
                  has_self = true;
                }
              }
            }

            sb_append_len(sb, mangled, mangled_len);
            sb_append(sb, "(");

            f->aux = (AstNode *)has_self;
            f->aux2 = n->as.func_call.args;
            f->flags = 2;

            if (has_self) {
              f->step = 1;
              if (top >= cap) {
                cap *= 2;
                stack = realloc(stack, sizeof(IterFrame) * cap);
                f = &stack[top - 1];
              }
              stack[top++] =
                  (IterFrame){member_node->as.member.base, 0, NULL, NULL, 0};
            } else {
              f->step = 2;
              f->aux = f->aux2;
              f->aux2 = NULL;
            }
          }
        } else if (f->step == 1) {
          if (f->aux2) {
            sb_append(sb, ", ");
          }
          f->aux = f->aux2;
          f->aux2 = NULL;
          f->step = 2;
        } else if (f->step == 2) {
          if (f->aux) {
            AstNode *arg = f->aux;
            f->aux = f->aux->next;
            f->step = (f->aux != NULL) ? 3 : 4;
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){arg, 0, NULL, NULL, 0};
          } else {
            sb_append(sb, ")");
            top--;
          }
        } else if (f->step == 3) {
          if (f->aux) {
            sb_append(sb, ", ");
          }
          f->step = 2;
        } else if (f->step == 4) {
          sb_append(sb, ")");
          top--;
        }
      } else {
        if (f->step == 0) {
          bool is_ctor = false;
          if (n->as.func_call.caller &&
              n->as.func_call.caller->type == AST_IDENTIF) {
            Sym *sym = n->as.func_call.caller->as.identif.res_sm;
            if (sym && (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION ||
                        sym->kind == SYM_ENUM)) {
              is_ctor = true;
            }
          }
          f->flags = is_ctor ? 1 : 0;
          if (f->flags)
            sb_append(sb, "(");
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.func_call.caller, 0, NULL, NULL, 0};
        } else if (f->step == 1) {
          if (f->flags)
            sb_append(sb, "){");
          else
            sb_append(sb, "(");
          f->aux = n->as.func_call.args;
          f->step = 2;
        } else if (f->step == 2) {
          if (f->aux) {
            AstNode *arg = f->aux;
            f->aux = f->aux->next;
            f->step = (f->aux != NULL) ? 3 : 4;
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){arg, 0, NULL, NULL, 0};
          } else {
            sb_append(sb, f->flags ? "}" : ")");
            top--;
          }
        } else if (f->step == 3) {
          sb_append(sb, ", ");
          f->step = 2;
        } else if (f->step == 4) {
          sb_append(sb, f->flags ? "}" : ")");
          top--;
        }
      }
    } else if (n->type == AST_ARRAY_LIT) {
      if (f->step == 0) {
        sb_append(sb, "{");
        f->aux = n->as.array_lit.elements;
        f->step = 1;
      } else if (f->step == 1) {
        if (f->aux) {
          AstNode *elem = f->aux;
          f->aux = f->aux->next;
          f->step = f->aux ? 2 : 3;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){elem, 0, NULL, NULL, 0};
        } else {
          sb_append(sb, "}");
          top--;
        }
      } else if (f->step == 2) {
        sb_append(sb, ", ");
        f->step = 1;
      } else {
        sb_append(sb, "}");
        top--;
      }
    } else if (n->type == AST_RET) {
      if (f->step == 0) {
        sb_append(sb, "return ");
        if (n->as.ret_stmt.expr) {
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.ret_stmt.expr, 0, NULL, NULL, 0};
        } else {
          sb_append(sb, ";\n");
          top--;
        }
      } else {
        sb_append(sb, ";\n");
        top--;
      }
    } else if (n->type == AST_CAST) {
      if (f->step == 0) {
        sb_append(sb, "(");
        gen_type(n->as.cast.target, sb);
        sb_append(sb, ")(");
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.cast.op, 0, NULL, NULL, 0};
      } else {
        sb_append(sb, ")");
        top--;
      }
    } else if (n->type == AST_MEMBER) {
      if (f->step == 0) {
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.member.base, 0, NULL, NULL, 0};
      } else {
        AstNode *base = n->as.member.base;
        if (base->eval_type.ptr_depth > 0) {
          sb_append(sb, "->");
        } else {
          sb_append(sb, ".");
        }
        sb_append_len(sb, n->as.member.name.start, n->as.member.name.len);
        top--;
      }
    } else if (n->type == AST_INDEX) {
      if (f->step == 0) {
        f->step = 1;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.index.base, 0, NULL, NULL, 0};
      } else if (f->step == 1) {
        sb_append(sb, "[");
        f->step = 2;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.index.index, 0, NULL, NULL, 0};
      } else {
        sb_append(sb, "]");
        top--;
      }
    } else if (n->type == AST_STRUCT || n->type == AST_UNION ||
               n->type == AST_ENUM) {
      bool is_enum = (n->type == AST_ENUM);
      bool is_nested = n->is_nested_sue;

      if (f->step == 0) {
        if (is_nested) {
          sb_append(sb, is_enum                   ? "enum {\n"
                        : (n->type == AST_STRUCT) ? "struct {\n"
                                                  : "union {\n");
        } else {
          Token tag = is_enum                   ? n->as.enum_def.enumn
                      : (n->type == AST_STRUCT) ? n->as.struct_def.structn
                                                : n->as.union_def.unionn;
          sb_append(sb, "typedef ");
          sb_append(sb, is_enum                   ? "enum "
                        : (n->type == AST_STRUCT) ? "struct "
                                                  : "union ");
          sb_append_len(sb, tag.start, tag.len);
          sb_append(sb, " ");
          sb_append_len(sb, tag.start, tag.len);
          sb_append(sb, ";\n");
          sb_append(sb, is_enum                   ? "enum "
                        : (n->type == AST_STRUCT) ? "struct "
                                                  : "union ");
          sb_append_len(sb, tag.start, tag.len);
          sb_append(sb, " {\n");
        }
        f->flags = is_nested ? 1 : 0;
        f->aux = is_enum                   ? n->as.enum_def.contents
                 : (n->type == AST_STRUCT) ? n->as.struct_def.contents
                                           : n->as.union_def.contents;
        f->step = 1;
      } else if (f->step == 1) {
        if (f->aux) {
          AstNode *member = f->aux;
          f->aux = f->aux->next;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){member, 0, NULL, NULL, 0};
          f->step = 2;
        } else {
          f->step = 3;
        }
      } else if (f->step == 2) {
        f->step = 1;
      } else {
        if (f->flags) {
          Token tag = is_enum                   ? n->as.enum_def.enumn
                      : (n->type == AST_STRUCT) ? n->as.struct_def.structn
                                                : n->as.union_def.unionn;
          sb_append(sb, "} ");
          sb_append_len(sb, tag.start, tag.len);
          sb_append(sb, ";\n");
        } else {
          sb_append(sb, "};\n");
        }
        top--;
      }
    } else if (n->type == AST_ENUM_MEMBER) {
      if (f->step == 0) {
        sb_append_len(sb, n->as.enum_member.name.start,
                      n->as.enum_member.name.len);
        if (n->as.enum_member.val) {
          sb_append(sb, " = ");
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.enum_member.val, 0, NULL, NULL, 0};
        } else {
          sb_append(sb, ",\n");
          top--;
        }
      } else {
        sb_append(sb, ",\n");
        top--;
      }
    } else if (n->type == AST_NUM_LIT) {
      sb_append_len(sb, n->as.num_lit.val.start, n->as.num_lit.val.len);
      top--;
    } else if (n->type == AST_IDENTIF) {
      sb_append_len(sb, n->as.identif.val.start, n->as.identif.val.len);
      top--;
    } else if (n->type == AST_STR_LIT) {
      sb_append_len(sb, n->as.str_lit.val.start, n->as.str_lit.val.len);
      top--;
    } else if (n->type == AST_BOOL_LIT) {
      sb_append_len(sb, n->as.bool_lit.val.start, n->as.bool_lit.val.len);
      top--;
    } else if (n->type == AST_CHAR_LIT) {
      sb_append_len(sb, n->as.char_lit.val.start, n->as.char_lit.val.len);
      top--;
    } else if (n->type == AST_NULL_LIT) {
      sb_append(sb, "NULL");
      top--;
    } else if (n->type == AST_BREAK) {
      sb_append(sb, "break;\n");
      top--;
    } else if (n->type == AST_CONTINUE) {
      sb_append(sb, "continue;\n");
      top--;
    } else {
      top--;
    }
  }

  free(stack);
}

void flatten_sues(AstNode *root, Arena *arena) {
  if (!root || root->type != AST_PROGRAM)
    return;

  size_t cap = 1024;
  FlattenFrame *stack = malloc(sizeof(FlattenFrame) * cap);
  size_t top = 0;

  stack[top++] = (FlattenFrame){root, (Token){0}};

  while (top > 0) {
    FlattenFrame frame = stack[--top];
    AstNode *n = frame.node;
    Token sue = frame.sue;

    if (!n)
      continue;

    if (n->type == AST_STRUCT || n->type == AST_UNION || n->type == AST_ENUM) {
      // Current SUE token for mangling and self replacement
      sue = (n->type == AST_STRUCT)  ? n->as.struct_def.structn
            : (n->type == AST_UNION) ? n->as.union_def.unionn
                                     : n->as.enum_def.enumn;

      // If inside another SUE, mark as nested
      if (frame.sue.len > 0)
        n->is_nested_sue = true;

      // Walk contents, hoisting functions right after this node
      AstNode **prev_ptr = (n->type == AST_STRUCT)  ? &n->as.struct_def.contents
                           : (n->type == AST_UNION) ? &n->as.union_def.contents
                                                    : &n->as.enum_def.contents;

      while (*prev_ptr) {
        AstNode *child = *prev_ptr;
        if (child->type == AST_FUNC) {
          // Mangle the method name
          Token old_name = child->as.func_def.fn_name;
          if (sue.len > 0 && old_name.len > 0) {
            size_t new_len = sue.len + 1 + old_name.len;
            char *new_name = arena_alloc(arena, new_len + 1);
            memcpy(new_name, sue.start, sue.len);
            new_name[sue.len] = '_';
            memcpy(new_name + sue.len + 1, old_name.start, old_name.len);
            new_name[new_len] = '\0';
            child->as.func_def.fn_name.start = new_name;
            child->as.func_def.fn_name.len = new_len;
          }

          // Remove from the SUEs contents list
          *prev_ptr = child->next;
          child->next = NULL;

          // Insert the function right after the SUE node in the top level list
          AstNode *after_sue = n->next;
          n->next = child;
          child->next = after_sue;

          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){child, sue};
        } else {
          if (child->type == AST_STRUCT || child->type == AST_UNION ||
              child->type == AST_ENUM)
            child->is_nested_sue = true;

          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){child, sue};
          prev_ptr = &child->next;
        }
      }
    } else if (n->type == AST_IDENTIF) {
      if (n->as.identif.val.len == 4 &&
          strncmp(n->as.identif.val.start, "self", 4) == 0 && sue.len > 0) {
        n->as.identif.val = sue;
      }
    } else {
      switch (n->type) {
      case AST_PROGRAM:
      case AST_BLOCK: {
        AstNode *stmt = n->as.block.first_stmt;
        while (stmt) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){stmt, sue};
          stmt = stmt->next;
        }
        break;
      }
      case AST_FUNC:
        if (n->as.func_def.params) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.func_def.params, sue};
        }
        if (n->as.func_def.block) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.func_def.block, sue};
        }
        break;
      case AST_PARAM:
        if (n->as.fn_param.id.len == 4 &&
            strncmp(n->as.fn_param.id.start, "self", 4) == 0 && sue.len > 0)
          n->as.fn_param.id = sue;
        break;
      case AST_VAR_DECL:
        if (n->as.var_decl.id.len == 4 &&
            strncmp(n->as.var_decl.id.start, "self", 4) == 0 && sue.len > 0)
          n->as.var_decl.id = sue;
        // Auto‑pointer for self‑referential fields
        if (sue.len > 0) {
          DataType *ft = &n->as.var_decl.type;
          if (ft->name.len == sue.len &&
              strncmp(ft->name.start, sue.start, sue.len) == 0 &&
              ft->ptr_depth == 0 && ft->array_dimens == 0)
            ft->ptr_depth = 1;
        }
        if (n->as.var_decl.init) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.var_decl.init, sue};
        }
        break;
      case AST_BINOP:
        if (top + 2 >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.binop.right, sue};
        stack[top++] = (FlattenFrame){n->as.binop.left, sue};
        break;
      case AST_UOP:
      case AST_ADDR_OF:
      case AST_DEREF:
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.unop.operand, sue};
        break;
      case AST_IF:
        if (n->as.if_check.elseAct) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.if_check.elseAct, sue};
        }
        if (n->as.if_check.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.if_check.action, sue};
        }
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.if_check.check, sue};
        break;
      case AST_WHILE:
        if (n->as.while_loop.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.while_loop.action, sue};
        }
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.while_loop.check, sue};
        break;
      case AST_FOR:
        if (n->as.for_loop.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.for_loop.action, sue};
        }
        if (n->as.for_loop.inc) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.for_loop.inc, sue};
        }
        if (n->as.for_loop.check) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.for_loop.check, sue};
        }
        if (n->as.for_loop.init) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.for_loop.init, sue};
        }
        break;
      case AST_FUNC_CALL: {
        AstNode *arg = n->as.func_call.args;
        while (arg) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){arg, sue};
          arg = arg->next;
        }
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.func_call.caller, sue};
        break;
      }
      case AST_ARRAY_LIT: {
        AstNode *elem = n->as.array_lit.elements;
        while (elem) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){elem, sue};
          elem = elem->next;
        }
        break;
      }
      case AST_INDEX:
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.index.index, sue};
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.index.base, sue};
        break;
      case AST_MEMBER:
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.member.base, sue};
        break;
      case AST_RET:
        if (n->as.ret_stmt.expr) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.ret_stmt.expr, sue};
        }
        break;
      case AST_DEFER:
        if (n->as.defer_stmt.contents) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.defer_stmt.contents, sue};
        }
        break;
      case AST_SWITCH:
        if (n->as.switch_stmt.default_case) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.switch_stmt.default_case, sue};
        }
        {
          AstNode *c = n->as.switch_stmt.cases;
          while (c) {
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(FlattenFrame) * cap);
            }
            stack[top++] = (FlattenFrame){c, sue};
            c = c->next;
          }
        }
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.switch_stmt.check, sue};
        break;
      case AST_CASE:
        if (n->as.case_stmt.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){n->as.case_stmt.action, sue};
        }
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.case_stmt.val, sue};
        break;
      case AST_EXTERN: {
        AstNode *stmt = n->as.extern_block.contents;
        while (stmt) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(FlattenFrame) * cap);
          }
          stack[top++] = (FlattenFrame){stmt, sue};
          stmt = stmt->next;
        }
        break;
      }
      case AST_CAST:
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.cast.op, sue};
        break;
      default:
        break;
      }
    }
  }

  free(stack);
}

bool output_to_c_and_compile(AstNode *root, const char *out_binary_name,
                             const char **flags, int flag_count, Arena *arena) {
  if (!root)
    return false;

  flatten_sues(root, arena);

  HashMap *func_map = build_func_map(arena, root);

  StringBuilder code;
  sb_init(&code);

  // Standard libs
  sb_append(&code, "/* Auto-generated by Tereix Transpiler */\n");
  sb_append(&code, "#include <stdio.h>\n");
  sb_append(&code, "#include <stdlib.h>\n");
  sb_append(&code, "#include <stdint.h>\n");
  sb_append(&code, "#include <stdbool.h>\n");
  sb_append(&code, "#include <string.h>\n\n");

  generate_c_code(root, &code, func_map, arena);

  const char *tmp_c_file = "output_gen.c";
  FILE *f = fopen(tmp_c_file, "w");
  if (!f) {
    fprintf(stderr, "Failed to create C output file.\n");
    sb_free(&code);
    return false;
  }
  fwrite(code.buf, 1, code.len, f);
  fclose(f);
  sb_free(&code);

  StringBuilder cmd;
  sb_init(&cmd);

  sb_append(&cmd, "gcc -o ");
  sb_append(&cmd, out_binary_name);
  sb_append(&cmd, " ");
  sb_append(&cmd, tmp_c_file);

  for (int i = 0; i < flag_count; i++) {
    sb_append(&cmd, " ");
    sb_append(&cmd, flags[i]);
  }

  printf("Executing: %s\n", cmd.buf);

  // TODO: Change to fork and exec later?
  int res = system(cmd.buf);
  sb_free(&cmd);

  return res == 0;
}

void compile_project(const char *entry_file) {
  Arena arena = {0};
  SemCtx sem = {0};
  sem_init(&sem, &arena);

  Worklist pending = {0};
  wl_push(&pending, entry_file);

  const char *current_path;
  while ((current_path = wl_pop(&pending)) != NULL) {
    const char *abs_path = resolve_alloc(&arena, current_path);
    if (!abs_path || map_get(&sem.mod_cache, abs_path, strlen(abs_path)))
      continue;

    printf("Compiling %s\n", abs_path);
    AstNode *ast = file_to_ast(&arena, abs_path);
    if (!ast) {
      fprintf(stderr, "No ast found after trying to parse %s", abs_path);
      exit(1);
    }

    const char *mod_name = extract_mod_name(&arena, abs_path);
    Module *mod = new_mod(&arena, abs_path, mod_name, ast);

    printf("Module: %s\n", mod_name);
    print_ast(ast);

    // Add to global cache
    map_set(&sem.mod_cache, abs_path, strlen(abs_path), mod);

    // Push dependencies
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

  // Collect local symbols
  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      collect_mod_symbols(&arena, mod);
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

      // Reset the scope stack count for each new module so scopes dont bleed
      // over from the previous module.
      ss.count = 0;

      // Resolve scopes for this specific modules AST
      resolve_scopes(&arena, mod, &ss);

      entry = entry->next;
    }
  }

  printf("Scope resolution complete.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;

      // Post-order type propagation
      type_check_ast(&arena, mod->ast_root);

      entry = entry->next;
    }
  }
  printf("Type checking complete.\n");

  const char *abs_path = resolve_alloc(&arena, entry_file);

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
            output_to_c_and_compile(mod->ast_root, bin_name, flags, 5, &arena);
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
