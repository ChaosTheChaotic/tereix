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

typedef struct {
  AstNode *src;
  AstNode *dst;
} ClonePair;

static inline bool clone_child(AstNode *src_child, AstNode **dst_child_ptr,
                               Arena *arena, ClonePair **stack, size_t *top,
                               size_t *cap) {
  if (!src_child)
    return true; // nothing to clone

  AstNode *dst = arena_alloc(arena, sizeof(AstNode));
  if (!dst)
    return false;

  *dst = *src_child;
  dst->next = NULL;
  *dst_child_ptr = dst;

  if (*top >= *cap) {
    size_t new_cap = *cap * 2;
    ClonePair *new_stack = realloc(*stack, sizeof(ClonePair) * new_cap);
    if (!new_stack) {
      fprintf(stderr,
              "OOM encountered whilst enlarging stack in cloning AST.\n");
      free(*stack);
      return false;
    }
    *stack = new_stack;
    *cap = new_cap;
  }

  (*stack)[(*top)++] = (ClonePair){src_child, dst};
  return true;
}

AstNode *clone_ast(AstNode *root, Arena *arena) {
  if (!root)
    return NULL;

  AstNode *new_root = arena_alloc(arena, sizeof(AstNode));
  *new_root = *root;
  new_root->next = NULL;

  size_t cap = 256;
  ClonePair *stack = malloc(sizeof(ClonePair) * cap);
  size_t top = 0;
  stack[top++] = (ClonePair){root, new_root};

  while (top > 0) {
    ClonePair p = stack[--top];
    AstNode *src = p.src;
    AstNode *dst = p.dst;

    if (!clone_child(src->next, &dst->next, arena, &stack, &top, &cap))
      return NULL;

    switch (src->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
      if (!clone_child(src->as.block.first_stmt, &dst->as.block.first_stmt,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_FUNC:
      if (!clone_child(src->as.func_def.params, &dst->as.func_def.params, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.func_def.block, &dst->as.func_def.block, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_VAR_DECL:
      if (!clone_child(src->as.var_decl.init, &dst->as.var_decl.init, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_BINOP:
      if (!clone_child(src->as.binop.left, &dst->as.binop.left, arena, &stack,
                       &top, &cap))
        return NULL;
      if (!clone_child(src->as.binop.right, &dst->as.binop.right, arena, &stack,
                       &top, &cap))
        return NULL;
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      if (!clone_child(src->as.unop.operand, &dst->as.unop.operand, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_IF:
      if (!clone_child(src->as.if_check.check, &dst->as.if_check.check, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.if_check.action, &dst->as.if_check.action, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.if_check.elseAct, &dst->as.if_check.elseAct,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_WHILE:
      if (!clone_child(src->as.while_loop.check, &dst->as.while_loop.check,
                       arena, &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.while_loop.action, &dst->as.while_loop.action,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_FOR:
      if (!clone_child(src->as.for_loop.init, &dst->as.for_loop.init, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.for_loop.check, &dst->as.for_loop.check, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.for_loop.inc, &dst->as.for_loop.inc, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.for_loop.action, &dst->as.for_loop.action, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_FUNC_CALL:
      if (!clone_child(src->as.func_call.caller, &dst->as.func_call.caller,
                       arena, &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.func_call.args, &dst->as.func_call.args, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_ARRAY_LIT:
      if (!clone_child(src->as.array_lit.elements, &dst->as.array_lit.elements,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_INDEX:
      if (!clone_child(src->as.index.base, &dst->as.index.base, arena, &stack,
                       &top, &cap))
        return NULL;
      if (!clone_child(src->as.index.index, &dst->as.index.index, arena, &stack,
                       &top, &cap))
        return NULL;
      break;
    case AST_MEMBER:
      if (!clone_child(src->as.member.base, &dst->as.member.base, arena, &stack,
                       &top, &cap))
        return NULL;
      break;
    case AST_RET:
      if (!clone_child(src->as.ret_stmt.expr, &dst->as.ret_stmt.expr, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_DEFER:
      if (!clone_child(src->as.defer_stmt.contents,
                       &dst->as.defer_stmt.contents, arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_SWITCH:
      if (!clone_child(src->as.switch_stmt.check, &dst->as.switch_stmt.check,
                       arena, &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.switch_stmt.cases, &dst->as.switch_stmt.cases,
                       arena, &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.switch_stmt.default_case,
                       &dst->as.switch_stmt.default_case, arena, &stack, &top,
                       &cap))
        return NULL;
      break;
    case AST_CASE:
      if (!clone_child(src->as.case_stmt.val, &dst->as.case_stmt.val, arena,
                       &stack, &top, &cap))
        return NULL;
      if (!clone_child(src->as.case_stmt.action, &dst->as.case_stmt.action,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_EXTERN:
      if (!clone_child(src->as.extern_block.contents,
                       &dst->as.extern_block.contents, arena, &stack, &top,
                       &cap))
        return NULL;
      break;
    case AST_CAST:
      if (!clone_child(src->as.cast.op, &dst->as.cast.op, arena, &stack, &top,
                       &cap))
        return NULL;
      break;
    case AST_STRUCT:
      if (!clone_child(src->as.struct_def.contents,
                       &dst->as.struct_def.contents, arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_UNION:
      if (!clone_child(src->as.union_def.contents, &dst->as.union_def.contents,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_ENUM:
      if (!clone_child(src->as.enum_def.contents, &dst->as.enum_def.contents,
                       arena, &stack, &top, &cap))
        return NULL;
      break;
    case AST_ENUM_MEMBER:
      if (!clone_child(src->as.enum_member.val, &dst->as.enum_member.val, arena,
                       &stack, &top, &cap))
        return NULL;
      break;
    case AST_SIZEOF:
      if (!clone_child(src->as.sizeof_expr.target_expr,
                       &dst->as.sizeof_expr.target_expr, arena, &stack, &top,
                       &cap))
        return NULL;
      break;
    default:
      break;
    }
#undef CLONE_CHILD
  }
  free(stack);
  return new_root;
}

bool ast_is_expr_node(const AstNode *node) {
  if (!node)
    return false;

  switch (node->type) {
  case AST_BINOP:
  case AST_UOP:
  case AST_NUM_LIT:
  case AST_STR_LIT:
  case AST_IDENTIF:
  case AST_FUNC_CALL:
  case AST_MEMBER:
  case AST_INDEX:
  case AST_CAST:
  case AST_BOOL_LIT:
  case AST_CHAR_LIT:
  case AST_NULL_LIT:
  case AST_ARRAY_LIT:
  case AST_ADDR_OF:
  case AST_DEREF:
  case AST_SIZEOF:
    return true;
  default:
    return false;
  }
}

bool ast_is_expr_ctx(const AstNode *parent, const AstNode *child) {
  if (!parent)
    return false;

  switch (parent->type) {
    // Any child is an expr
  case AST_BINOP:
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
  case AST_FUNC_CALL:
  case AST_ARRAY_LIT:
  case AST_RET:
  case AST_CAST:
  case AST_MEMBER:
  case AST_INDEX:
  case AST_ENUM_MEMBER:
  case AST_SIZEOF:
    return true;

  // Contexts where only specific children are expressions
  case AST_CASE:
    return parent->as.case_stmt.val == child;
  case AST_SWITCH:
    return parent->as.switch_stmt.check == child;
  case AST_VAR_DECL:
    return parent->as.var_decl.init == child;
  case AST_IF:
    return parent->as.if_check.check == child;
  case AST_WHILE:
    return parent->as.while_loop.check == child;
  case AST_FOR:
    return (parent->as.for_loop.check == child) ||
           (parent->as.for_loop.inc == child);
  default:
    return false;
  }
}
