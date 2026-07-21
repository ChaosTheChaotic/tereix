#include "ast_types.h"
#include "ast_visitor.h"
#include "lex_types.h"
#include "parse_types.h"
#include "util.h"
#include <errno.h>
#include <stdarg.h>

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
  if (!node)
    return NULL;
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
  if (!new_stmt)
    return;
  new_stmt->next = NULL;
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

  int panic_status = setjmp(pctx.panic_env);
  if (panic_status != ERR_NONE) {
    map_free_buckets(&lex.kw_map);
    map_free_buckets(&lex.op_map);
    map_free_buckets(&lex.comp_map);
    map_free_buckets(&lex.type_kw_map);
    free(pctx.state_stack);
    free(pctx.node_stack);
    free(pctx.op_stack);

    diaglist_add(diag_list, DIAG_ERROR,
                 "Compiler panicked and aborted parsing.", fpath, 1, 1, 1, 1);

    return NULL;
  }

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

static inline bool safe_fprintf(FILE *fp, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int ret = vfprintf(fp, fmt, args);
  va_end(args);

  if (ret < 0) {
    fprintf(stderr, "printing type info failed: %s\n", strerror(errno));
    fclose(fp);
    return false;
  }
  return true;
}

static inline bool safe_fputc(FILE *fp, int ch) {
  if (fputc(ch, fp) == EOF) {
    fprintf(stderr, "printing type info failed: %s\n", strerror(errno));
    fclose(fp);
    return false;
  }
  return true;
}

bool print_type_info(DataType type, FILE *out_fp) {
  if (type.ptr_depth != 0) {
    char symbol = (type.ptr_depth < 0) ? '*' : '&';
    int count = (type.ptr_depth > 0) ? type.ptr_depth : -type.ptr_depth;

    for (int i = 0; i < count; i++) {
      if (!safe_fputc(out_fp, symbol))
        return false;
    }
    if (!safe_fputc(out_fp, ' '))
      return false;
  }

  if (type.is_static) {
    if (!safe_fprintf(out_fp, "static "))
      return false;
  }
  if (type.is_mut) {
    if (!safe_fprintf(out_fp, "mut "))
      return false;
  }
  if (type.is_threadlocal) {
    if (!safe_fprintf(out_fp, "threadlocal "))
      return false;
  }
  if (type.is_extern) {
    if (!safe_fprintf(out_fp, "extern "))
      return false;
  }

  if (!safe_fprintf(out_fp, "%.*s", type.name.len, type.name.start))
    return false;

  for (unsigned int i = 0; i < type.array_dimens; i++) {
    if (type.dim_sizes != NULL && type.dim_sizes[i] != NULL) {
      AstNode *dim_node = type.dim_sizes[i];
      if (dim_node->type == AST_NUM_LIT) {
        if (!safe_fprintf(out_fp, "[%.*s]", dim_node->as.num_lit.val.len,
                          dim_node->as.num_lit.val.start))
          return false;
      } else {
        if (!safe_fprintf(out_fp, "[expr]"))
          return false;
      }
    } else {
      if (!safe_fprintf(out_fp, "[]"))
        return false;
    }
  }
  return true;
}

VisitResult print_ast_enter(AstVisitor *visitor, AstNode *node) {
  int *depth = (int *)visitor->user_data;

  for (int i = 0; i < *depth; i++)
    printf("  | ");

  switch (node->type) {
  case AST_PROGRAM:
    printf("PROGRAM\n");
    break;

  case AST_FUNC:
    if (node->as.func_def.is_extern)
      printf("EXTERN ");
    if (node->as.func_def.is_async)
      printf("ASYNC ");
    if (node->as.func_def.is_inline)
      printf("INLINE ");
    printf("FUNC (Return: ");

    if (!print_type_info(node->as.func_def.ret_type, stdout)) {
      fprintf(stderr, "Failed to print type info, returning early\n");
      return VISIT_ABORT;
    }

    printf("): %.*s\n", node->as.func_def.fn_name.len,
           node->as.func_def.fn_name.start);

    if (!node->as.func_def.block) {
      for (int i = 0; i < (*depth) + 1; i++)
        printf("  | ");
      printf("[Prototype]\n");
    }
    break;

  case AST_PARAM:
    printf("PARAM (");
    if (!print_type_info(node->as.fn_param.type, stdout)) {
      fprintf(stderr, "failed to print type info, returning early\n");
      return VISIT_ABORT;
    }
    printf("): %.*s\n", node->as.fn_param.id.len, node->as.fn_param.id.start);
    break;

  case AST_VAR_DECL:
    printf("VAR_DECL (");
    if (!print_type_info(node->as.var_decl.type, stdout)) {
      fprintf(stderr, "failed to print type info, returning early\n");
      return VISIT_ABORT;
    }
    printf("): %.*s\n", node->as.var_decl.id.len, node->as.var_decl.id.start);
    break;

  case AST_BINOP:
    printf("BINOP (%.*s)\n", node->as.binop.op.len, node->as.binop.op.start);
    break;

  case AST_UOP:
    if (node->as.unop.is_postfix) {
      printf("POSTFIX_UOP (%.*s)\n", node->as.unop.op.len,
             node->as.unop.op.start);
    } else {
      printf("PREFIX_UOP (%.*s)\n", node->as.unop.op.len,
             node->as.unop.op.start);
    }
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
    if (node->as.block.is_async)
      printf("ASYNC ");
    printf("BLOCK\n");
    break;

  case AST_IF:
    printf("IF_STMT\n");
    break;

  case AST_RET:
    printf("RETURN\n");
    break;

  case AST_FOR:
    printf("FOR_LOOP\n");
    break;

  case AST_FUNC_CALL:
    printf("FUNC_CALL\n");
    break;

  case AST_WHILE:
    printf("WHILE_LOOP\n");
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
    break;

  case AST_ENUM_MEMBER:
    printf("ENUM_MEMBER: %.*s\n", node->as.enum_member.name.len,
           node->as.enum_member.name.start);
    break;

  case AST_ARRAY_LIT:
    printf("ARRAY_LIT\n");
    break;

  case AST_INDEX:
    printf("INDEX_ACCESS\n");
    break;

  case AST_MEMBER:
    printf("MEMBER_ACCESS: .%.*s\n", node->as.member.name.len,
           node->as.member.name.start);
    break;

  case AST_DEFER:
    printf("DEFER\n");
    break;

  case AST_STRUCT:
    printf("STRUCT: %.*s\n", node->as.struct_def.structn.len,
           node->as.struct_def.structn.start);
    break;

  case AST_UNION:
    printf("UNION: %.*s\n", node->as.union_def.unionn.len,
           node->as.union_def.unionn.start);
    break;

  case AST_ADDR_OF:
    printf("ADDRESS_OF\n");
    break;

  case AST_DEREF:
    printf("DEREF\n");
    break;

  case AST_EXTERN:
    printf("EXTERN_BLOCK\n");
    break;

  case AST_SWITCH:
    printf("SWITCH\n");
    break;

  case AST_CASE:
    printf("CASE\n");
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
      fprintf(stderr, "Failed to print type info, returning early\n");
      return VISIT_ABORT;
    }
    printf(")\n");
    break;

  default:
    printf("AST_NODE_TYPE_%d\n", node->type);
    break;
  }

  // Increment depth for child nodes
  (*depth)++;
  return VISIT_CONTINUE;
}

void print_ast_exit(AstVisitor *visitor, AstNode *node) {
  (void)node; // Unused
  // Decrement depth when moving back up the tree
  int *depth = (int *)visitor->user_data;
  (*depth)--;
}

void print_ast(AstNode *root) {
  if (!root)
    return;

  int depth = 0;
  AstVisitor visitor = {
      .user_data = &depth,
      .panic_env = NULL,
      .enter_node = print_ast_enter,
      .interleave_node = NULL,
      .exit_node = print_ast_exit,
  };

  ast_traverse(&visitor, root);
  printf("\n");
}
