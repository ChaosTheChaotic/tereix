#include "ast_types.h"
#include "lex_types.h"
#include "parse_types.h"
#include "util.h"
#include <errno.h>

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

  // Its not technically a keyword but its an inbuilt function
  map_set(&ctx->kw_map, "sizeof", 6, (void *)1);
}

AstNode *new_node(Arena *arena, ASTN_TYPE type) {
  AstNode *node = arena_alloc(arena, sizeof(AstNode));
  memset(node, 0, sizeof(AstNode));
  node->type = type;
  return node;
}

void write_ast(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "w")) != NULL) {
    // Write stuff here
    fclose(fp);
  } else {
    printf("Failed to open file to write AST");
  }
}

void append_stmt(AstNode **head, AstNode *new_stmt) {
  if (*head == NULL) {
    *head = new_stmt;
  } else {
    AstNode *tail = *head;
    while (tail->next != NULL) {
      tail = tail->next;
    }
    tail->next = new_stmt;
  }
}

AstNode *str_to_ast(Arena *arena, const char *file, const char *fpath,
                    DiagList *diag_list, bool partial) {
  LexCtx lex = {0};
  lex.start = (char *)file;
  lex.curr = (char *)file;
  lex.line = 1;
  lex.col = 1;
  lex.file = fpath;
  init_lex_maps(&lex, arena);

  ParseCtx pctx = {0};
  pctx.lex = &lex;
  pctx.arena = arena;
  pctx.state_cap = 64;
  pctx.state_stack = malloc(sizeof(ParseState) * pctx.state_cap);
  pctx.diags = diag_list;
  pctx.curr = next_token(&pctx);

  AstNode *root = new_node(arena, AST_PROGRAM);
  push_node(&pctx, root);

  bool success = parse(&pctx);
  map_free_buckets(&lex.kw_map);
  map_free_buckets(&lex.op_map);
  map_free_buckets(&lex.comp_map);
  map_free_buckets(&lex.type_kw_map);
  free(pctx.state_stack);
  free(pctx.node_stack);
  free(pctx.op_stack);

  if (!success && !partial) {
    return NULL;
  }
  return root;
}

AstNode *file_to_ast(Arena *arena, const char *path, bool partial) {
  const char *file = load_file(path);
  if (!file)
    return NULL;
  DiagList diags;
  diaglist_init(&diags, 1024);
  AstNode *root = str_to_ast(arena, file, path, &diags, partial);
  if (!partial) {
    for (size_t i = 0; i < diags.count; i++) {
      printf("Error on %u:%u in file %s: %s\n", diags.items[i].start_line,
             diags.items[i].start_char, diags.items[i].file,
             diags.items[i].message);
    }
  }
  diaglist_free(&diags);
  return root;
}

bool print_type_info(DataType type, FILE *out_fp) {
#define FPRINTF_SAFE(fmt, ...)                                                 \
  do {                                                                         \
    if (fprintf(out_fp, fmt, __VA_ARGS__) < 0) {                               \
      fprintf(stderr, "printing type info failed: %s\n", strerror(errno));     \
      fclose(out_fp);                                                          \
      return false;                                                            \
    }                                                                          \
  } while (0)
#define FPUTC_SAFE(ch)                                                         \
  do {                                                                         \
    if (fprintf(out_fp, "%c", ch) < 0) {                                       \
      fprintf(stderr, "printing type info failed: %s\n", strerror(errno));     \
      fclose(out_fp);                                                          \
      return false;                                                            \
    }                                                                          \
  } while (0)
  if (type.ptr_depth != 0) {
    char symbol = (type.ptr_depth < 0) ? '*' : '&';
    int count = (type.ptr_depth > 0) ? type.ptr_depth : -type.ptr_depth;

    for (int i = 0; i < count; i++) {
      FPUTC_SAFE(symbol);
    }
    FPUTC_SAFE(' ');
  }

  if (type.is_static)
    FPRINTF_SAFE("%s", "static ");
  if (type.is_mut)
    FPRINTF_SAFE("%s", "mut ");
  if (type.is_threadlocal)
    FPRINTF_SAFE("%s", "threadlocal ");
  if (type.is_extern)
    FPRINTF_SAFE("%s", "extern ");

  FPRINTF_SAFE("%.*s", type.name.len, type.name.start);

  for (unsigned int i = 0; i < type.array_dimens; i++) {
    if (type.dim_sizes != NULL && type.dim_sizes[i] != NULL) {
      AstNode *dim_node = type.dim_sizes[i];

      if (dim_node->type == AST_NUM_LIT) {
        FPRINTF_SAFE("[%.*s]", dim_node->as.num_lit.val.len,
                     dim_node->as.num_lit.val.start);
      } else {
        FPRINTF_SAFE("%s", "[expr]");
      }
    } else {
      FPRINTF_SAFE("%s", "[]");
    }
  }
  return true;
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

    for (unsigned int i = 0; i < item.depth; i++) {
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

    unsigned int next_depth = item.depth + 1;

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
      if (!print_type_info(node->as.func_def.ret_type, stdout)) {
        fprintf(stderr, "Failed to print type info, returning early");
        return;
      }
      printf("): %.*s\n", node->as.func_def.fn_name.len,
             node->as.func_def.fn_name.start);

      if (node->as.func_def.block) {
        stack[top++] =
            (AstPrintItem){node->as.func_def.block, next_depth, "Body"};
      } else {
        for (unsigned int i = 0; i < next_depth; i++)
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
      if (!print_type_info(node->as.fn_param.type, stdout)) {
        fprintf(stderr, "failed to print type info, returning early");
        return;
      }
      printf("): %.*s\n", node->as.fn_param.id.len, node->as.fn_param.id.start);
      break;

    case AST_VAR_DECL:
      printf("VAR_DECL (");
      if (!print_type_info(node->as.var_decl.type, stdout)) {
        fprintf(stderr, "failed to print type info, returning early");
        return;
      }
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
      if (!print_type_info(node->as.cast.target, stdout)) {
        fprintf(stderr, "Failed to print type info, returning early");
        return;
      }
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
