#include "arena.h"
#include "ast_visitor.h"
#include "c_gen_types.h"
#include "hashutils.h"
#include "parse_types.h"
#include "sem_types.h"
#include "string_builder.h"
#include "util.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

void gen_type(DataType type, StringBuilder *sb) {
  if (type.is_static)
    sb_append(sb, "static ");
  if (type.is_threadlocal)
    sb_append(sb, "_Thread_local ");
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

  for (long i = 0; i < type.ptr_depth; i++)
    sb_append(sb, "*");
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

bool is_c_expr(AstNode *n) {
  if (!n)
    return false;
  return n->type == AST_BINOP || n->type == AST_UOP || n->type == AST_NUM_LIT ||
         n->type == AST_STR_LIT || n->type == AST_IDENTIF ||
         n->type == AST_FUNC_CALL || n->type == AST_MEMBER ||
         n->type == AST_INDEX || n->type == AST_CAST ||
         n->type == AST_BOOL_LIT || n->type == AST_CHAR_LIT ||
         n->type == AST_NULL_LIT || n->type == AST_ARRAY_LIT ||
         n->type == AST_ADDR_OF || n->type == AST_DEREF ||
         n->type == AST_SIZEOF;
}

typedef struct {
  const char *var_name;
  Arena *arena;
  AstNode *parent_stack[2048];
  size_t parent_top;
} InjectYieldCtx;

VisitResult inject_yield_enter(AstVisitor *v, AstNode *n) {
  InjectYieldCtx *ctx = v->user_data;
  AstNode *parent =
      ctx->parent_top > 0 ? ctx->parent_stack[ctx->parent_top - 1] : NULL;

  // Only follow exec tail
  if (parent) {
    if (parent->type == AST_BLOCK) {
      // Only process the very last statement in a block
      if (n->next != NULL)
        return VISIT_SKIP_CHILDREN;
    } else if (parent->type == AST_IF) {
      if (n == parent->as.if_check.check)
        return VISIT_SKIP_CHILDREN;
    } else if (parent->type == AST_SWITCH) {
      if (n == parent->as.switch_stmt.check)
        return VISIT_SKIP_CHILDREN;
    } else if (parent->type == AST_CASE) {
      if (n == parent->as.case_stmt.val)
        return VISIT_SKIP_CHILDREN;
    } else {
      // Any other parent relationship
      return VISIT_SKIP_CHILDREN;
    }
  }

  // Push current node to context parent stack
  if (ctx->parent_top >= 2048) {
    if (v->panic_env)
      longjmp(*v->panic_env, ERR_OOM);
    return VISIT_ABORT;
  }
  ctx->parent_stack[ctx->parent_top++] = n;

  if (n->type == AST_BLOCK || n->type == AST_IF || n->type == AST_SWITCH ||
      n->type == AST_CASE) {
    return VISIT_CONTINUE;
  }

  if (n->type == AST_BREAK || n->type == AST_CONTINUE || n->type == AST_RET ||
      n->type == AST_DEFER || n->type == AST_VAR_DECL) {
    return VISIT_SKIP_CHILDREN;
  }

  // Inject assignment
  AstNode *target = arena_alloc(ctx->arena, sizeof(AstNode));
  memset(target, 0, sizeof(AstNode));
  target->type = AST_IDENTIF;

  size_t vlen = strlen(ctx->var_name);
  char *vstr = arena_alloc(ctx->arena, vlen + 1);
  strcpy(vstr, ctx->var_name);

  target->as.identif.val.start = vstr;
  target->as.identif.val.len = vlen;
  target->eval_type = n->eval_type;

  // Save the original expression and clear its next pointer to isolate it
  AstNode *original_expr = arena_alloc(ctx->arena, sizeof(AstNode));
  *original_expr = *n;
  original_expr->next = NULL;

  // Backup the chain
  AstNode *next_node = n->next;

  // Mutate n in place to become the assignment operator
  memset(n, 0, sizeof(AstNode));
  n->type = AST_BINOP;
  n->as.binop.op.start = "=";
  n->as.binop.op.len = 1;
  n->as.binop.left = target;
  n->as.binop.right = original_expr;
  n->eval_type = original_expr->eval_type;

  // Restore chain linking
  n->next = next_node;

  // Dont go deeper
  return VISIT_SKIP_CHILDREN;
}

void inject_yield_exit(AstVisitor *v, AstNode *n) {
  InjectYieldCtx *ctx = v->user_data;
  // Only pop if node was pushed
  if (ctx->parent_top > 0 && ctx->parent_stack[ctx->parent_top - 1] == n) {
    ctx->parent_top--;
  }
}

void inject_yield_assignments(AstNode *node, const char *var_name,
                              Arena *arena) {
  if (!node)
    return;

  InjectYieldCtx ctx = {0};
  ctx.var_name = var_name;
  ctx.arena = arena;
  ctx.parent_top = 0;

  AstVisitor visitor = {0};
  visitor.user_data = &ctx;
  visitor.enter_node = inject_yield_enter;
  visitor.exit_node = inject_yield_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, node);
  } else {
    fprintf(stderr, "OOM encountered whilst injecting yield assignments.\n");
  }
}

typedef struct {
  StringBuilder *sb;
  HashMap *func_map;
  Arena *arena;
  bool is_main_mod;
  uint64_t yield_blk_ctr;

  AstNode *parent_stack[2048];
  size_t parent_top;
} GenCtx;

// Check if node evaluated as expr
bool is_expr_context(AstNode *parent, AstNode *child) {
  if (!parent)
    return false;
  switch (parent->type) {
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
    return parent->as.for_loop.check == child ||
           parent->as.for_loop.inc == child;
  default:
    return false;
  }
}

// Checks if an expression block can be unwrapped safely
static bool is_auto_unwrap(AstNode *n) {
  if (n->type != AST_BLOCK)
    return false;
  AstNode *unwrapped = n;
  while (unwrapped && unwrapped->type == AST_BLOCK &&
         unwrapped->as.block.first_stmt &&
         !unwrapped->as.block.first_stmt->next) {
    AstNode *stmt = unwrapped->as.block.first_stmt;
    if (is_c_expr(stmt))
      unwrapped = stmt;
    else
      break;
  }
  return unwrapped != n;
}

VisitResult gen_enter(AstVisitor *v, AstNode *n) {
  GenCtx *ctx = v->user_data;
  StringBuilder *sb = ctx->sb;
  AstNode *parent =
      ctx->parent_top > 0 ? ctx->parent_stack[ctx->parent_top - 1] : NULL;

  ctx->parent_stack[ctx->parent_top++] = n;

  // Block auto unwrap handling (skip standard block structure)
  if (n->type == AST_BLOCK && is_expr_context(parent, n) && is_auto_unwrap(n)) {
    AstNode *unwrapped = n;
    while (unwrapped && unwrapped->type == AST_BLOCK &&
           unwrapped->as.block.first_stmt &&
           !unwrapped->as.block.first_stmt->next) {
      AstNode *stmt = unwrapped->as.block.first_stmt;
      if (is_c_expr(stmt))
        unwrapped = stmt;
      else
        break;
    }
    // Temporarily spoof parent context to bypass expression wrapper rules
    AstNode *old_parent = ctx->parent_stack[ctx->parent_top - 1];
    ctx->parent_stack[ctx->parent_top - 1] = parent;
    ast_traverse(v, unwrapped);
    ctx->parent_stack[ctx->parent_top - 1] = old_parent;
    return VISIT_SKIP_CHILDREN;
  }

  // Context dependent items
  if (parent) {
    if (parent->type == AST_BINOP && n == parent->as.binop.right) {
      sb_append(sb, " ");
      sb_append_len(sb, parent->as.binop.op.start, parent->as.binop.op.len);
      sb_append(sb, " ");
    } else if (parent->type == AST_INDEX && n == parent->as.index.index) {
      sb_append(sb, "[");
    } else if (parent->type == AST_CAST && n == parent->as.cast.op) {
      sb_append(sb, ")(");
    }
  }

  switch (n->type) {
  case AST_BLOCK:
    sb_append(sb, "{\n");
    break;

  case AST_FUNC: {
    if (n->as.func_def.fn_name.len == 4 &&
        strncmp(n->as.func_def.fn_name.start, "main", 4) == 0 &&
        ctx->is_main_mod) {
      AstNode *params = n->as.func_def.params;
      int param_count = 0;
      AstNode *p = params;
      while (p) {
        param_count++;
        p = p->next;
      }

      if (param_count != 2) {
        fprintf(stderr,
                "Error: main function must have exactly 2 parameters (an "
                "integer and a string array). Found %d.\n",
                param_count);
        return VISIT_SKIP_CHILDREN;
      }

      AstNode *param1 = params;
      DataType t1 = param1->as.fn_param.type;
      int width;
      bool is_signed, is_float;
      if (!get_numeric_info(t1, &width, &is_signed, &is_float) || width < 32) {
        fprintf(stderr, "Error: first parameter of main must be a integer with "
                        "at least 32 bits (e.g. i32, i64).\n");
        return VISIT_SKIP_CHILDREN;
      }

      AstNode *param2 = param1->next;
      DataType t2 = param2->as.fn_param.type;
      if (!(t2.name.len == 3 && strncmp(t2.name.start, "str", 3) == 0 &&
            (t2.ptr_depth + t2.array_dimens >= 1))) {
        fprintf(stderr,
                "Error: second parameter of main must be str[] or **str.\n");
        return VISIT_SKIP_CHILDREN;
      }

      if (n->as.func_def.is_extern)
        sb_append(sb, "extern ");
      if (n->as.func_def.is_inline)
        sb_append(sb, "inline ");
      sb_append(sb, "int main(int ");
      sb_append_len(sb, param1->as.fn_param.id.start,
                    param1->as.fn_param.id.len);
      sb_append(sb, ", char **");
      sb_append_len(sb, param2->as.fn_param.id.start,
                    param2->as.fn_param.id.len);
      sb_append(sb, ") ");
    } else {
      if (n->as.func_def.is_extern)
        sb_append(sb, "extern ");
      if (n->as.func_def.is_inline)
        sb_append(sb, "inline ");

      DataType clean_ret = n->as.func_def.ret_type;
      clean_ret.is_extern = false;
      clean_ret.is_static = false;
      if (clean_ret.ptr_depth == 0 && clean_ret.array_dimens == 0)
        clean_ret.is_mut = true;
      if (clean_ret.array_dimens > 0) {
        // C doesn't allow array return types so treat as pointer to element
        clean_ret.ptr_depth += clean_ret.array_dimens;
        clean_ret.array_dimens = 0;
      }

      gen_type(clean_ret, sb);
      sb_append_len(sb, n->as.func_def.fn_name.start,
                    n->as.func_def.fn_name.len);
      sb_append(sb, "(");

      AstNode *p = n->as.func_def.params;
      while (p) {
        gen_type(p->as.fn_param.type, sb);
        sb_append_len(sb, p->as.fn_param.id.start, p->as.fn_param.id.len);
        p = p->next;
        if (p)
          sb_append(sb, ", ");
      }
      sb_append(sb, ") ");
    }
    if (n->as.func_def.block)
      ast_traverse(v, n->as.func_def.block);
    else
      sb_append(sb, ";\n");

    return VISIT_SKIP_CHILDREN;
  }

  case AST_VAR_DECL: {
    DataType decl_type = n->as.var_decl.type;
    long saved_dims = decl_type.array_dimens;
    decl_type.array_dimens = 0;
    gen_type(decl_type, sb);
    decl_type.array_dimens = saved_dims;

    sb_append_len(sb, n->as.var_decl.id.start, n->as.var_decl.id.len);

    for (unsigned int i = 0; i < decl_type.array_dimens; i++) {
      sb_append(sb, "[");
      if (decl_type.dim_sizes[i]) {
        ast_traverse(v, decl_type.dim_sizes[i]);
      }
      sb_append(sb, "]");
    }

    if (n->as.var_decl.init) {
      sb_append(sb, " = ");
      ast_traverse(v, n->as.var_decl.init);
    }
    return VISIT_SKIP_CHILDREN;
  }

  case AST_BINOP:
    sb_append(sb, "(");
    break;

  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
    if (n->type == AST_ADDR_OF)
      sb_append(sb, "&(");
    else if (n->type == AST_DEREF)
      sb_append(sb, "*(");
    else {
      if (!n->as.unop.is_postfix)
        sb_append_len(sb, n->as.unop.op.start, n->as.unop.op.len);
      sb_append(sb, "(");
    }
    break;

  case AST_IF:
    sb_append(sb, "if (");
    ast_traverse(v, n->as.if_check.check);
    sb_append(sb, ") ");
    ast_traverse(v, n->as.if_check.action);
    if (n->as.if_check.elseAct) {
      sb_append(sb, " else ");
      ast_traverse(v, n->as.if_check.elseAct);
    }
    return VISIT_SKIP_CHILDREN;

  case AST_WHILE:
    sb_append(sb, "while (");
    ast_traverse(v, n->as.while_loop.check);
    sb_append(sb, ") ");
    ast_traverse(v, n->as.while_loop.action);
    return VISIT_SKIP_CHILDREN;

  case AST_FOR:
    sb_append(sb, "for (");
    if (n->as.for_loop.init)
      ast_traverse(v, n->as.for_loop.init);
    if (!n->as.for_loop.init || n->as.for_loop.init->type != AST_VAR_DECL)
      sb_append(sb, "; ");

    if (n->as.for_loop.check)
      ast_traverse(v, n->as.for_loop.check);
    sb_append(sb, "; ");

    if (n->as.for_loop.inc)
      ast_traverse(v, n->as.for_loop.inc);
    sb_append(sb, ") ");

    if (n->as.for_loop.action)
      ast_traverse(v, n->as.for_loop.action);
    return VISIT_SKIP_CHILDREN;

  case AST_FUNC_CALL: {
    bool is_ctor = false;
    Sym *sym = NULL;

    if (n->as.func_call.caller && n->as.func_call.caller->type == AST_MEMBER) {
      AstNode *member_node = n->as.func_call.caller;
      AstNode *base = member_node->as.member.base;

      if (base->type == AST_IDENTIF && base->as.identif.res_sm &&
          base->as.identif.res_sm->is_imported_mod) {
        sb_append_len(sb, base->as.identif.val.start, base->as.identif.val.len);
        sb_append(sb, "_");
        sb_append_len(sb, member_node->as.member.name.start,
                      member_node->as.member.name.len);
        sb_append(sb, "(");
      } else {
        Token method = member_node->as.member.name;
        DataType base_eval = base->eval_type;
        Token base_type = base_eval.name;
        if (base_type.len == 0)
          base_type = member_node->as.member.type;

        size_t mangled_len = base_type.len + 1 + method.len;
        char *mangled = arena_alloc(ctx->arena, mangled_len + 1);
        memcpy(mangled, base_type.start, base_type.len);
        mangled[base_type.len] = '_';
        memcpy(mangled + base_type.len + 1, method.start, method.len);
        mangled[mangled_len] = '\0';

        AstNode *func_def = map_get(ctx->func_map, mangled, mangled_len);
        bool has_self = false;
        bool needs_addrof = false;
        bool needs_deref = false;

        if (func_def && func_def->type == AST_FUNC) {
          AstNode *first_param = func_def->as.func_def.params;
          if (first_param) {
            DataType pt = first_param->as.fn_param.type;
            if (pt.name.len == base_type.len &&
                strncmp(pt.name.start, base_type.start, pt.name.len) == 0 &&
                pt.array_dimens == 0) {
              has_self = true;
              if (pt.ptr_depth > base_eval.ptr_depth)
                needs_addrof = true;
              else if (pt.ptr_depth < base_eval.ptr_depth)
                needs_deref = true;
            }
          }
        }

        sb_append_len(sb, mangled, mangled_len);
        sb_append(sb, "(");

        if (has_self) {
          AstNode *self_arg = base;
          if (needs_addrof) {
            AstNode addrof = {0};
            addrof.type = AST_ADDR_OF;
            addrof.as.unop.operand = self_arg;
            ast_traverse(v, &addrof);
          } else if (needs_deref) {
            AstNode deref = {0};
            deref.type = AST_DEREF;
            deref.as.unop.operand = self_arg;
            ast_traverse(v, &deref);
          } else {
            ast_traverse(v, self_arg);
          }
          if (n->as.func_call.args)
            sb_append(sb, ", ");
        }
      }
    } else {
      if (n->as.func_call.caller &&
          n->as.func_call.caller->type == AST_IDENTIF) {
        sym = n->as.func_call.caller->as.identif.res_sm;
        if (sym && (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION ||
                    sym->kind == SYM_ENUM))
          is_ctor = true;
      }
      if (is_ctor) {
        if (sym->kind == SYM_STRUCT)
          sb_append(sb, "(struct ");
        else if (sym->kind == SYM_UNION)
          sb_append(sb, "(union ");
        else
          sb_append(sb, "(enum ");
      }

      ast_traverse(v, n->as.func_call.caller);
      sb_append(sb, is_ctor ? "){" : "(");
    }

    AstNode *arg = n->as.func_call.args;
    while (arg) {
      ast_traverse(v, arg);
      arg = arg->next;
      if (arg)
        sb_append(sb, ", ");
    }
    sb_append(sb, is_ctor ? "}" : ")");
    return VISIT_SKIP_CHILDREN;
  }

  case AST_ARRAY_LIT: {
    DataType t = n->eval_type;
    if (t.name.len > 0) {
      sb_append(sb, "(");
      int array_lit_depth = 0;
      for (int i = (int)ctx->parent_top - 1; i >= 0; i--) {
        if (ctx->parent_stack[i]->type == AST_ARRAY_LIT)
          array_lit_depth++;
        else
          break;
      }

      int total = t.ptr_depth + t.array_dimens;
      total -= array_lit_depth;
      if (total < 0)
        total = 0;

      t.ptr_depth = total > 0 ? total - 1 : 0;
      t.array_dimens = 0;
      gen_type(t, sb);
      sb_append(sb, "[])");
    }
    sb_append(sb, "{");

    AstNode *elem = n->as.array_lit.elements;
    while (elem) {
      ast_traverse(v, elem);
      elem = elem->next;
      if (elem)
        sb_append(sb, ", ");
    }
    sb_append(sb, "}");
    return VISIT_SKIP_CHILDREN;
  }

  case AST_RET:
    if (n->as.ret_stmt.expr && n->as.ret_stmt.expr->type == AST_BLOCK) {
      AstNode *block = n->as.ret_stmt.expr;
      AstNode *unwrapped = block;
      while (unwrapped && unwrapped->type == AST_BLOCK &&
             unwrapped->as.block.first_stmt &&
             !unwrapped->as.block.first_stmt->next) {
        AstNode *stmt = unwrapped->as.block.first_stmt;
        if (is_c_expr(stmt))
          unwrapped = stmt;
        else
          break;
      }

      if (unwrapped->type != AST_BLOCK) {
        sb_append(sb, "return ");
        ast_traverse(v, unwrapped);
        sb_append(sb, ";\n");
      } else {
        bool is_void = (block->eval_type.name.len == 4 &&
                        strncmp(block->eval_type.name.start, "void", 4) == 0 &&
                        block->eval_type.ptr_depth == 0 &&
                        block->eval_type.array_dimens == 0) ||
                       (block->eval_type.name.len == 0);
        char var_name[64];
        sprintf(var_name, "_tx_blk_%zu", ++ctx->yield_blk_ctr);

        if (!is_void) {
          DataType mut_type = block->eval_type;
          mut_type.is_mut = true;
          mut_type.is_threadlocal = false;
          mut_type.is_static = false;
          mut_type.is_extern = false;
          gen_type(mut_type, sb);
          sb_append(sb, var_name);
          sb_append(sb, ";\n");
          inject_yield_assignments(block, var_name, ctx->arena);
        }

        AstNode *stmt = block->as.block.first_stmt;
        while (stmt) {
          ast_traverse(v, stmt);
          stmt = stmt->next;
        }

        if (!is_void) {
          sb_append(sb, "return ");
          sb_append(sb, var_name);
          sb_append(sb, ";\n");
        } else {
          sb_append(sb, "return;\n");
        }
      }
    } else {
      sb_append(sb, "return ");
      if (n->as.ret_stmt.expr)
        ast_traverse(v, n->as.ret_stmt.expr);
      sb_append(sb, ";\n");
    }
    return VISIT_SKIP_CHILDREN;

  case AST_CAST:
    sb_append(sb, "(");
    gen_type(n->as.cast.target, sb);
    sb_append(sb, ")(");
    break;

  case AST_STRUCT:
  case AST_UNION:
  case AST_ENUM: {
    bool is_enum = (n->type == AST_ENUM);
    bool is_nested = n->is_nested_sue;
    bool is_opaque = (n->type == AST_STRUCT && !n->as.struct_def.contents) ||
                     (n->type == AST_UNION && !n->as.union_def.contents);

    Token tag = is_enum ? n->as.enum_def.enumn
                        : (n->type == AST_STRUCT ? n->as.struct_def.structn
                                                 : n->as.union_def.unionn);

    if (is_nested) {
      sb_append(sb, is_enum ? "enum "
                            : (n->type == AST_STRUCT ? "struct " : "union "));
      sb_append_len(sb, tag.start, tag.len);
      sb_append(sb, " {\n");
    } else {
      if (is_opaque && !is_enum) {
        sb_append(sb, "typedef ");
        sb_append(sb, n->type == AST_STRUCT ? "struct " : "union ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, " ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, ";\n");
        return VISIT_SKIP_CHILDREN;
      }

      if (is_enum) {
        sb_append(sb, "typedef enum ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, " {\n");
      } else {
        sb_append(sb, "typedef ");
        sb_append(sb, n->type == AST_STRUCT ? "struct " : "union ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, " ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, ";\n");
        sb_append(sb, n->type == AST_STRUCT ? "struct " : "union ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, " {\n");
      }
    }

    AstNode *child = is_enum
                         ? n->as.enum_def.contents
                         : (n->type == AST_STRUCT ? n->as.struct_def.contents
                                                  : n->as.union_def.contents);
    while (child) {
      ast_traverse(v, child);
      child = child->next;
    }

    if (is_nested) {
      sb_append(sb, "};\n");
    } else {
      if (is_enum) {
        sb_append(sb, "} ");
        sb_append_len(sb, tag.start, tag.len);
        sb_append(sb, ";\n");
      } else
        sb_append(sb, "};\n");
    }
    return VISIT_SKIP_CHILDREN;
  }

  case AST_ENUM_MEMBER:
    sb_append_len(sb, n->as.enum_member.name.start, n->as.enum_member.name.len);
    if (n->as.enum_member.val) {
      sb_append(sb, " = ");
      ast_traverse(v, n->as.enum_member.val);
    }
    sb_append(sb, ",\n");
    return VISIT_SKIP_CHILDREN;

  case AST_NUM_LIT:
    sb_append_len(sb, n->as.num_lit.val.start, n->as.num_lit.val.len);
    break;
  case AST_IDENTIF:
    sb_append_len(sb, n->as.identif.val.start, n->as.identif.val.len);
    break;
  case AST_STR_LIT:
    sb_append_len(sb, n->as.str_lit.val.start, n->as.str_lit.val.len);
    break;
  case AST_BOOL_LIT:
    sb_append_len(sb, n->as.bool_lit.val.start, n->as.bool_lit.val.len);
    break;
  case AST_CHAR_LIT:
    sb_append_len(sb, n->as.char_lit.val.start, n->as.char_lit.val.len);
    break;
  case AST_NULL_LIT:
    sb_append(sb, "NULL");
    break;
  case AST_BREAK:
    sb_append(sb, "break;\n");
    break;
  case AST_CONTINUE:
    sb_append(sb, "continue;\n");
    break;

  case AST_SWITCH:
    sb_append(sb, "switch (");
    ast_traverse(v, n->as.switch_stmt.check);
    sb_append(sb, ") {\n");

    AstNode *c = n->as.switch_stmt.cases;
    while (c) {
      ast_traverse(v, c);
      c = c->next;
    }
    if (n->as.switch_stmt.default_case)
      ast_traverse(v, n->as.switch_stmt.default_case);
    sb_append(sb, "}\n");
    return VISIT_SKIP_CHILDREN;

  case AST_CASE:
    if (n->as.case_stmt.val) {
      sb_append(sb, "case ");
      ast_traverse(v, n->as.case_stmt.val);
      sb_append(sb, ":\n");
    } else
      sb_append(sb, "default:\n");
    if (n->as.case_stmt.action)
      ast_traverse(v, n->as.case_stmt.action);
    return VISIT_SKIP_CHILDREN;

  case AST_SIZEOF:
    sb_append(sb, "sizeof(");
    if (n->as.sizeof_expr.is_type) {
      gen_type(n->as.sizeof_expr.target_type, sb);
      sb_append(sb, ")");
    } else {
      ast_traverse(v, n->as.sizeof_expr.target_expr);
      sb_append(sb, ")");
    }
    return VISIT_SKIP_CHILDREN;

  default:
    break;
  }

  return VISIT_CONTINUE;
}

void gen_exit(AstVisitor *v, AstNode *n) {
  GenCtx *ctx = v->user_data;
  StringBuilder *sb = ctx->sb;
  AstNode *parent =
      ctx->parent_top > 1 ? ctx->parent_stack[ctx->parent_top - 2] : NULL;

  switch (n->type) {
  case AST_BLOCK:
    if (!(is_expr_context(parent, n) && is_auto_unwrap(n))) {
      sb_append(sb, "}\n");
    }
    break;
  case AST_BINOP:
    sb_append(sb, ")");
    break;
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
    sb_append(sb, ")");
    if (n->type == AST_UOP && n->as.unop.is_postfix)
      sb_append_len(sb, n->as.unop.op.start, n->as.unop.op.len);
    break;
  case AST_CAST:
    sb_append(sb, ")");
    break;
  case AST_MEMBER:
    if (n->as.member.base->eval_type.ptr_depth > 0)
      sb_append(sb, "->");
    else
      sb_append(sb, ".");
    sb_append_len(sb, n->as.member.name.start, n->as.member.name.len);
    break;
  case AST_INDEX:
    sb_append(sb, "]");
    break;
  default:
    break;
  }

  // Top level semicolons are implicitly added for parent items
  if (parent && (parent->type == AST_BLOCK || parent->type == AST_PROGRAM ||
                 parent->type == AST_EXTERN)) {
    bool needs_semi = (n->type == AST_FUNC_CALL || n->type == AST_BINOP ||
                       n->type == AST_UOP || n->type == AST_IDENTIF ||
                       n->type == AST_NUM_LIT || n->type == AST_STR_LIT ||
                       n->type == AST_MEMBER || n->type == AST_VAR_DECL);
    if (needs_semi)
      sb_append(sb, ";\n");
  }

  ctx->parent_top--;
}

void generate_c_code(AstNode *root, StringBuilder *sb, HashMap *func_map,
                     Arena *arena, bool is_main_mod) {
  GenCtx data = {0};
  data.sb = sb;
  data.func_map = func_map;
  data.arena = arena;
  data.is_main_mod = is_main_mod;
  data.yield_blk_ctr = 0;
  data.parent_top = 0;

  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = gen_enter;
  visitor.exit_node = gen_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered during C code generation.\n");
  }
}

typedef struct {
  Arena *arena;
  Token sue_stack[256];
  int sue_top;
} FlattenData;

VisitResult flatten_enter(AstVisitor *visitor, AstNode *n) {
  FlattenData *data = visitor->user_data;
  Token sue =
      data->sue_top > 0 ? data->sue_stack[data->sue_top - 1] : (Token){0};

  if (n->type == AST_STRUCT || n->type == AST_UNION || n->type == AST_ENUM) {
    Token my_sue = (n->type == AST_STRUCT)  ? n->as.struct_def.structn
                   : (n->type == AST_UNION) ? n->as.union_def.unionn
                                            : n->as.enum_def.enumn;
    if (sue.len > 0)
      n->is_nested_sue = true;
    data->sue_stack[data->sue_top++] = my_sue;
  } else if (n->type == AST_IDENTIF) {
    if (n->as.identif.val.len == 4 &&
        strncmp(n->as.identif.val.start, "self", 4) == 0 && sue.len > 0) {
      n->as.identif.val = sue;
      if (!n->as.identif.res_sm) {
        n->as.identif.res_sm = arena_alloc(data->arena, sizeof(Sym));
      }
      n->as.identif.res_sm->kind = SYM_STRUCT;
    }
  } else if (n->type == AST_PARAM) {
    if (n->as.fn_param.id.len == 4 &&
        (n->as.fn_param.id.start == NULL ||
         strncmp(n->as.fn_param.id.start, "self", 4) == 0) &&
        sue.len > 0) {
      n->as.fn_param.id = sue;
      n->as.fn_param.type.name = sue;
    }
  } else if (n->type == AST_VAR_DECL) {
    if (n->as.var_decl.id.len == 4 &&
        (n->as.var_decl.id.start == NULL ||
         strncmp(n->as.var_decl.id.start, "self", 4) == 0) &&
        sue.len > 0)
      n->as.var_decl.id = sue;
    if (sue.len > 0) {
      DataType *ft = &n->as.var_decl.type;
      if (ft->name.len == sue.len &&
          strncmp(ft->name.start, sue.start, sue.len) == 0 &&
          ft->ptr_depth == 0 && ft->array_dimens == 0)
        ft->ptr_depth = 1;
    }
  } else if (n->type == AST_MEMBER) {
    AstNode *base = n->as.member.base;
    if (base && base->type == AST_IDENTIF && base->as.identif.res_sm &&
        base->as.identif.res_sm->is_imported_mod) {
      Token mod_name = base->as.identif.val;
      Token mem_name = n->as.member.name;
      size_t new_len = mod_name.len + 1 + mem_name.len;
      char *new_name = arena_alloc(data->arena, new_len + 1);
      sprintf(new_name, "%.*s_%.*s", (int)mod_name.len, mod_name.start,
              (int)mem_name.len, mem_name.start);
      n->type = AST_IDENTIF;
      n->as.identif.val.start = new_name;
      n->as.identif.val.len = new_len;
      n->as.identif.res_sm = NULL;
    }
  }

  return VISIT_CONTINUE;
}

void flatten_exit(AstVisitor *visitor, AstNode *n) {
  FlattenData *data = visitor->user_data;

  if (n->type == AST_STRUCT || n->type == AST_UNION || n->type == AST_ENUM) {
    Token sue = data->sue_stack[--data->sue_top]; // Pop context

    AstNode **prev_ptr = (n->type == AST_STRUCT)  ? &n->as.struct_def.contents
                         : (n->type == AST_UNION) ? &n->as.union_def.contents
                                                  : &n->as.enum_def.contents;

    while (*prev_ptr) {
      AstNode *child = *prev_ptr;
      if (child->type == AST_FUNC) {
        Token old_name = child->as.func_def.fn_name;
        if (sue.len > 0 && old_name.len > 0) {
          size_t new_len = sue.len + 1 + old_name.len;
          char *new_name = arena_alloc(data->arena, new_len + 1);
          memcpy(new_name, sue.start, sue.len);
          new_name[sue.len] = '_';
          memcpy(new_name + sue.len + 1, old_name.start, old_name.len);
          new_name[new_len] = '\0';
          child->as.func_def.fn_name.start = new_name;
          child->as.func_def.fn_name.len = new_len;
        }

        // Hoist function
        *prev_ptr = child->next;
        child->next = NULL;

        AstNode *after_sue = n->next;
        n->next = child;
        child->next = after_sue;
      } else {
        if (child->type == AST_STRUCT || child->type == AST_UNION ||
            child->type == AST_ENUM) {
          child->is_nested_sue = true;
        }
        prev_ptr = &child->next;
      }
    }
  }
}

void flatten_sues(AstNode *root, Arena *arena) {
  if (!root || root->type != AST_PROGRAM)
    return;

  FlattenData data = {arena, {{0}}, 0};
  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = flatten_enter;
  visitor.exit_node = flatten_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered whilst flattening SUEs.\n");
  }
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

#define CLONE_CHILD(child_ptr)                                                 \
  if (src->child_ptr) {                                                        \
    dst->child_ptr = arena_alloc(arena, sizeof(AstNode));                      \
    *dst->child_ptr = *src->child_ptr;                                         \
    dst->child_ptr->next = NULL;                                               \
    if (top >= cap) {                                                          \
      size_t new_cap = cap * 2;                                                \
      ClonePair *new_stack = realloc(stack, sizeof(ClonePair) * new_cap);      \
      if (!new_stack) {                                                        \
        fprintf(stderr,                                                        \
                "OOM encountered whilst enlargig stack in cloning AST.\n");    \
        free(stack);                                                           \
        return NULL;                                                           \
      }                                                                        \
      stack = new_stack;                                                       \
      cap = new_cap;                                                           \
    }                                                                          \
    stack[top++] = (ClonePair){src->child_ptr, dst->child_ptr};                \
  }

    CLONE_CHILD(next)

    switch (src->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
      CLONE_CHILD(as.block.first_stmt);
      break;
    case AST_FUNC:
      CLONE_CHILD(as.func_def.params);
      CLONE_CHILD(as.func_def.block);
      break;
    case AST_VAR_DECL:
      CLONE_CHILD(as.var_decl.init);
      break;
    case AST_BINOP:
      CLONE_CHILD(as.binop.left);
      CLONE_CHILD(as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      CLONE_CHILD(as.unop.operand);
      break;
    case AST_IF:
      CLONE_CHILD(as.if_check.check);
      CLONE_CHILD(as.if_check.action);
      CLONE_CHILD(as.if_check.elseAct);
      break;
    case AST_WHILE:
      CLONE_CHILD(as.while_loop.check);
      CLONE_CHILD(as.while_loop.action);
      break;
    case AST_FOR:
      CLONE_CHILD(as.for_loop.init);
      CLONE_CHILD(as.for_loop.check);
      CLONE_CHILD(as.for_loop.inc);
      CLONE_CHILD(as.for_loop.action);
      break;
    case AST_FUNC_CALL:
      CLONE_CHILD(as.func_call.caller);
      CLONE_CHILD(as.func_call.args);
      break;
    case AST_ARRAY_LIT:
      CLONE_CHILD(as.array_lit.elements);
      break;
    case AST_INDEX:
      CLONE_CHILD(as.index.base);
      CLONE_CHILD(as.index.index);
      break;
    case AST_MEMBER:
      CLONE_CHILD(as.member.base);
      break;
    case AST_RET:
      CLONE_CHILD(as.ret_stmt.expr);
      break;
    case AST_DEFER:
      CLONE_CHILD(as.defer_stmt.contents);
      break;
    case AST_SWITCH:
      CLONE_CHILD(as.switch_stmt.check);
      CLONE_CHILD(as.switch_stmt.cases);
      CLONE_CHILD(as.switch_stmt.default_case);
      break;
    case AST_CASE:
      CLONE_CHILD(as.case_stmt.val);
      CLONE_CHILD(as.case_stmt.action);
      break;
    case AST_EXTERN:
      CLONE_CHILD(as.extern_block.contents);
      break;
    case AST_CAST:
      CLONE_CHILD(as.cast.op);
      break;
    case AST_STRUCT:
      CLONE_CHILD(as.struct_def.contents);
      break;
    case AST_UNION:
      CLONE_CHILD(as.union_def.contents);
      break;
    case AST_ENUM:
      CLONE_CHILD(as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      CLONE_CHILD(as.enum_member.val);
      break;
    case AST_SIZEOF:
      CLONE_CHILD(as.sizeof_expr.target_expr);
      break;
    default:
      break;
    }
#undef CLONE_CHILD
  }
  free(stack);
  return new_root;
}

typedef struct LowerCtx {
  Arena *arena;
  AstNode **defers;
  size_t defer_count;
  size_t defer_cap;
  LowerFrame *frames;
  size_t frame_top;
  size_t frame_cap;
  uint64_t ret_id_ctr; // for generating unique temp variable names
  jmp_buf *panic_env;
} LowerCtx;

// Push defer into pool
void push_defer(LowerCtx *ctx, AstNode *defer) {
  if (ctx->defer_count >= ctx->defer_cap) {
    ctx->defer_cap = ctx->defer_cap ? ctx->defer_cap * 2 : 64;
    AstNode **new_defers =
        realloc(ctx->defers, sizeof(AstNode *) * ctx->defer_cap);
    if (!new_defers) {
      longjmp(*ctx->panic_env, ERR_OOM);
    }
    ctx->defers = new_defers;
  }
  ctx->defers[ctx->defer_count++] = defer;
}

void push_frame(LowerCtx *ctx, AstNode *n, size_t func_base, size_t loop_base,
                size_t block_base) {
  if (ctx->frame_top >= ctx->frame_cap) {
    ctx->frame_cap = ctx->frame_cap ? ctx->frame_cap * 2 : 64;
    LowerFrame *new_frames =
        realloc(ctx->frames, sizeof(LowerFrame) * ctx->frame_cap);
    if (!new_frames) {
      longjmp(*ctx->panic_env, ERR_OOM);
    }
    ctx->frames = new_frames;
  }
  ctx->frames[ctx->frame_top++] = (LowerFrame){.node = n,
                                               .func_base = func_base,
                                               .loop_base = loop_base,
                                               .block_base = block_base,
                                               .step = 0};
}

// Get the top frame for a given node or NULL if not found
LowerFrame *get_frame(LowerCtx *ctx, AstNode *n) {
  for (size_t i = ctx->frame_top; i > 0; --i) {
    if (ctx->frames[i - 1].node == n)
      return &ctx->frames[i - 1];
  }
  return NULL;
}

// Visitor enter callback
VisitResult lower_enter(AstVisitor *v, AstNode *n) {
  LowerCtx *ctx = v->user_data;

  // Get current frame
  LowerFrame *frame =
      ctx->frame_top > 0 ? &ctx->frames[ctx->frame_top - 1] : NULL;

  switch (n->type) {
  case AST_FUNC: {
    push_frame(ctx, n, ctx->defer_count, ctx->defer_count, ctx->defer_count);
    break;
  }
  case AST_BLOCK: {
    // Remove all AST_DEFER children from the block and store them
    AstNode **p = &n->as.block.first_stmt;
    while (*p) {
      if ((*p)->type == AST_DEFER) {
        push_defer(ctx, *p);
        *p = (*p)->next;
      } else {
        p = &(*p)->next;
      }
    }
    // Push a block frame with current defer count
    push_frame(ctx, n, frame ? frame->func_base : 0,
               frame ? frame->loop_base : 0, ctx->defer_count);
    break;
  }
  case AST_WHILE:
  case AST_FOR: {
    size_t func_base = frame ? frame->func_base : 0;
    size_t loop_base = ctx->defer_count;
    push_frame(ctx, n, func_base, loop_base, ctx->defer_count);
    break;
  }
  case AST_IF:
  case AST_SWITCH:
  case AST_CASE: {
    // These only need to preserve function/loop bases for control flow
    size_t func_base = frame ? frame->func_base : 0;
    size_t loop_base = frame ? frame->loop_base : 0;
    push_frame(ctx, n, func_base, loop_base, ctx->defer_count);
    break;
  }
  case AST_RET: {
    // Replace return with a block that runs defers then returns
    if (frame && ctx->defer_count > frame->func_base) {
      AstNode *blk = arena_alloc(ctx->arena, sizeof(AstNode));
      memset(blk, 0, sizeof(AstNode));
      blk->type = AST_BLOCK;

      AstNode *tail = NULL;
      // If the return has an expression store it in a temp var
      AstNode *ret_expr = n->as.ret_stmt.expr;
      AstNode *ret_val_ident = NULL;
      if (ret_expr) {
        AstNode *decl = arena_alloc(ctx->arena, sizeof(AstNode));
        memset(decl, 0, sizeof(AstNode));
        decl->type = AST_VAR_DECL;
        decl->as.var_decl.type = ret_expr->eval_type;
        decl->as.var_decl.type.is_threadlocal = false;
        decl->as.var_decl.type.is_static = false;
        decl->as.var_decl.type.is_extern = false;

        char tmp[64];
        snprintf(tmp, sizeof(tmp), "_tx_ret_%zu", ++ctx->ret_id_ctr);
        decl->as.var_decl.id =
            (Token){.start = arena_alloc(ctx->arena, strlen(tmp) + 1),
                    .len = strlen(tmp),
                    .type = TOKEN_IDENTIF};
        strcpy((char *)decl->as.var_decl.id.start, tmp);
        decl->as.var_decl.init = ret_expr;

        ret_val_ident = arena_alloc(ctx->arena, sizeof(AstNode));
        memset(ret_val_ident, 0, sizeof(AstNode));
        ret_val_ident->type = AST_IDENTIF;
        ret_val_ident->as.identif.val = decl->as.var_decl.id;

        blk->as.block.first_stmt = decl;
        tail = decl;
      }

      // Inject clones of defers in reverse
      for (size_t i = ctx->defer_count; i > frame->func_base; --i) {
        AstNode *cloned =
            clone_ast(ctx->defers[i - 1]->as.defer_stmt.contents, ctx->arena);
        if (!blk->as.block.first_stmt) {
          blk->as.block.first_stmt = cloned;
          tail = cloned;
        } else {
          tail->next = cloned;
        }
        while (tail->next)
          tail = tail->next;
      }

      // New return statement
      AstNode *new_ret = arena_alloc(ctx->arena, sizeof(AstNode));
      memset(new_ret, 0, sizeof(AstNode));
      new_ret->type = AST_RET;
      new_ret->as.ret_stmt.expr = ret_val_ident;

      if (!blk->as.block.first_stmt)
        blk->as.block.first_stmt = new_ret;
      else
        tail->next = new_ret;

      // Replace the current node with the block
      AstNode *next = n->next;
      *n = *blk;
      n->next = next;
    }
    break;
  }
  case AST_BREAK:
  case AST_CONTINUE: {
    // Run defers up to loop_base
    if (frame && ctx->defer_count > frame->loop_base) {
      AstNode *blk = arena_alloc(ctx->arena, sizeof(AstNode));
      memset(blk, 0, sizeof(AstNode));
      blk->type = AST_BLOCK;
      AstNode *tail = NULL;

      for (size_t i = ctx->defer_count; i > frame->loop_base; --i) {
        AstNode *cloned =
            clone_ast(ctx->defers[i - 1]->as.defer_stmt.contents, ctx->arena);
        if (!blk->as.block.first_stmt) {
          blk->as.block.first_stmt = cloned;
          tail = cloned;
        } else {
          tail->next = cloned;
        }
        while (tail->next)
          tail = tail->next;
      }

      AstNode *ctrl = arena_alloc(ctx->arena, sizeof(AstNode));
      memset(ctrl, 0, sizeof(AstNode));
      ctrl->type = n->type;
      if (n->type == AST_BREAK)
        ctrl->as.break_stmt = n->as.break_stmt;
      else
        ctrl->as.continue_stmt = n->as.continue_stmt;

      if (!blk->as.block.first_stmt)
        blk->as.block.first_stmt = ctrl;
      else
        tail->next = ctrl;

      AstNode *next = n->next;
      *n = *blk;
      n->next = next;
    }
    break;
  }
  default:
    break;
  }
  return VISIT_CONTINUE;
}

void lower_exit(AstVisitor *v, AstNode *n) {
  LowerCtx *ctx = v->user_data;
  LowerFrame *frame = get_frame(ctx, n);
  if (!frame)
    return;

  switch (n->type) {
  case AST_BLOCK: {
    // Inject defers registered at end
    AstNode *tail = n->as.block.first_stmt;
    while (tail && tail->next)
      tail = tail->next;

    for (size_t i = ctx->defer_count; i > frame->block_base; --i) {
      AstNode *cloned =
          clone_ast(ctx->defers[i - 1]->as.defer_stmt.contents, ctx->arena);
      if (!n->as.block.first_stmt) {
        n->as.block.first_stmt = cloned;
        tail = cloned;
      } else {
        tail->next = cloned;
      }
      while (tail->next)
        tail = tail->next;
    }
    // Remove defers from pool
    ctx->defer_count = frame->block_base;
    break;
  }
  case AST_FUNC:
  case AST_WHILE:
  case AST_FOR:
  case AST_IF:
  case AST_SWITCH:
  case AST_CASE:
    // Pop the frame
    ctx->frame_top--;
    break;
  default:
    break;
  }
}

void lower_defers(AstNode *root, Arena *arena) {
  if (!root)
    return;

  LowerCtx ctx = {.arena = arena,
                  .defers = NULL,
                  .defer_count = 0,
                  .defer_cap = 0,
                  .frames = NULL,
                  .frame_top = 0,
                  .frame_cap = 0,
                  .ret_id_ctr = 0};

  AstVisitor visitor = {0};
  visitor.user_data = &ctx;
  visitor.enter_node = lower_enter;
  visitor.exit_node = lower_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;
  ctx.panic_env = &panic_env;
  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered while lowering defers.\n");
  }

  free(ctx.defers);
  free(ctx.frames);
}

bool compile_from_memory(const char *compiler, const char **flags,
                         int flag_count, const char *out_binary_name,
                         StringBuilder *code) {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    perror("pipe");
    return false;
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("fork");
    return false;
  }

  if (pid == 0) {
    close(pipefd[1]);              // Close write end
    dup2(pipefd[0], STDIN_FILENO); // Bind read end to stdin
    close(pipefd[0]);

    // Construct command
    size_t args_alloc = 6 + flag_count + 1;
    char **argv = malloc(args_alloc * sizeof(char *));
    int argc = 0;
    argv[argc++] = (char *)compiler;
    argv[argc++] = "-x";
    argv[argc++] = "c";
    argv[argc++] = "-"; // Read from stdin
    argv[argc++] = "-o";
    argv[argc++] = (char *)out_binary_name;
    for (int i = 0; i < flag_count; i++) {
      argv[argc++] = (char *)flags[i];
    }
    argv[argc] = NULL;

    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
  } else {
    close(pipefd[0]); // Close read end

    // Push the entire AST string builder into the pipe
    size_t total_written = 0;
    while (total_written < code->len) {
      ssize_t written = write(pipefd[1], code->buf + total_written,
                              code->len - total_written);
      if (written == -1) {
        if (errno == EINTR)
          continue;
        perror("write to pipe");
        break;
      }
      total_written += written;
    }
    close(pipefd[1]);

    int status;
    if (waitpid(pid, &status, 0) == -1)
      return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
}

bool output_to_c_and_compile(SemCtx *sem, const char *out_binary_name,
                             const char *compiler, const char **flags,
                             int flag_count, Arena *arena, Module *main_mod,
                             bool keep_c_files) {
  if (!sem)
    return false;

  StringBuilder code;
  sb_init(&code);

  // Standard libs
  sb_append(&code, "/* Auto-generated by Tereix Transpiler */\n");
  sb_append(&code, "#include <stdint.h>\n");
  sb_append(&code, "#include <stddef.h>\n");
  sb_append(&code, "#include <stdbool.h>\n\n");

  HashMap *global_func_map = arena_alloc(arena, sizeof(HashMap));
  map_init(global_func_map, arena, 256);

  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;

      if (mod != main_mod)
        mangle_mod_symbols(arena, mod);
      lower_defers(mod->ast_root, arena);
      flatten_sues(mod->ast_root, arena);

      HashMap *local_funcs = build_func_map(arena, mod->ast_root);

      for (size_t j = 0; j < local_funcs->capacity; j++) {
        HashEntry *local_entry = local_funcs->buckets[j];
        while (local_entry) {
          map_set(global_func_map, local_entry->key, local_entry->key_len,
                  local_entry->value);
          local_entry = local_entry->next;
        }
      }
      map_free_buckets(local_funcs);
      entry = entry->next;
    }
  }

  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;

      sb_append(&code, "/* Module: ");
      sb_append(&code, mod->mod_name);
      sb_append(&code, " */\n");

      generate_c_code(mod->ast_root, &code, global_func_map, arena,
                      (mod == main_mod));

      entry = entry->next;
    }
  }

  uint64_t code_hash = hash_string(code.buf, code.len);
  char hash_file[512];
  snprintf(hash_file, sizeof(hash_file), ".tx_cache/%s.hash", out_binary_name);

  bool up_to_date = false;
  if (check_exists(out_binary_name)) {
    FILE *hf = fopen(hash_file, "rb");
    if (hf) {
      uint64_t stored_hash;
      if (fread(&stored_hash, sizeof(uint64_t), 1, hf) == 1 &&
          stored_hash == code_hash) {
        up_to_date = true;
      }
      fclose(hf);
    }
  }

  if (up_to_date) {
    printf("Generated C code unchanged and binary exists. Skipping "
           "compilation.\n");
    sb_free(&code);
    map_free_buckets(global_func_map);
    return true;
  }

  bool compile_success = false;

  if (!keep_c_files) {
    compile_success = compile_from_memory(compiler, flags, flag_count,
                                          out_binary_name, &code);
  } else {
    char c_file_path[512] = ".tx_cache/output_gen.c";
    FILE *f = fopen(c_file_path, "w");
    if (!f) {
      fprintf(stderr, "Failed to create C output file at %s.\n", c_file_path);
      sb_free(&code);
      map_free_buckets(global_func_map);
      return false;
    }
    fwrite(code.buf, 1, code.len, f);
    fclose(f);

    StringBuilder cmd;
    sb_init(&cmd);
    sb_append(&cmd, compiler);
    sb_append(&cmd, " -o ");
    sb_append(&cmd, out_binary_name);
    sb_append(&cmd, " ");
    sb_append(&cmd, c_file_path);

    for (int i = 0; i < flag_count; i++) {
      sb_append(&cmd, " ");
      sb_append(&cmd, flags[i]);
    }

    printf("Executing: %s\n", cmd.buf);

    char **argv = NULL;
    size_t argc = 0;
    char *cmd_copy = strdup(cmd.buf);
    if (!cmd_copy) {
      fprintf(stderr, "Failed to duplicate command string\n");
      sb_free(&cmd);
      sb_free(&code);
      map_free_buckets(global_func_map);
      return false;
    }

    char *token = strtok(cmd_copy, " ");
    while (token) {
      char **new_argv = realloc(argv, (argc + 2) * sizeof(char *));
      if (!new_argv) {
        fprintf(stderr, "Error: OOM while building compiler arguments.\n");
        free(cmd_copy);
        free(argv);
        sb_free(&cmd);
        sb_free(&code);
        map_free_buckets(global_func_map);
        return false;
      }
      argv = new_argv;
      argv[argc++] = token;
      token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    pid_t pid = fork();
    int res = -1;
    if (pid == -1) {
      perror("fork");
    } else if (pid == 0) {
      execvp(argv[0], argv);
      perror("execvp");
      _exit(127);
    } else {
      int status;
      if (waitpid(pid, &status, 0) != -1 && WIFEXITED(status)) {
        res = WEXITSTATUS(status);
      }
    }

    free(cmd_copy);
    free(argv);
    sb_free(&cmd);
    compile_success = (res == 0);
  }

  if (compile_success) {
    FILE *hf = fopen(hash_file, "wb");
    if (hf) {
      fwrite(&code_hash, sizeof(uint64_t), 1, hf);
      fclose(hf);
    }
  }

  sb_free(&code);
  map_free_buckets(global_func_map);
  return compile_success;
}

typedef struct MangleCtx {
  Arena *arena;
  HashMap *rename_map;
} MangleCtx;

// Rename a token if present
void rename_token(MangleCtx *ctx, Token *tok) {
  if (!tok || tok->len == 0)
    return;
  Token *replacement = map_get(ctx->rename_map, tok->start, tok->len);
  if (replacement) {
    *tok = *replacement;
  }
}

// Rename name of DataType
void rename_type(MangleCtx *ctx, DataType *t) {
  if (t)
    rename_token(ctx, &t->name);
}

VisitResult mangle_enter(AstVisitor *v, AstNode *n) {
  MangleCtx *ctx = v->user_data;

  // Always rename eval_type name
  rename_type(ctx, &n->eval_type);

  switch (n->type) {
  case AST_FUNC:
    rename_token(ctx, &n->as.func_def.fn_name);
    rename_type(ctx, &n->as.func_def.ret_type);
    break;
  case AST_VAR_DECL:
    rename_token(ctx, &n->as.var_decl.id);
    rename_type(ctx, &n->as.var_decl.type);
    break;
  case AST_STRUCT:
    rename_token(ctx, &n->as.struct_def.structn);
    break;
  case AST_UNION:
    rename_token(ctx, &n->as.union_def.unionn);
    break;
  case AST_ENUM:
    rename_token(ctx, &n->as.enum_def.enumn);
    break;
  case AST_ENUM_MEMBER:
    rename_token(ctx, &n->as.enum_member.name);
    break;
  case AST_PARAM:
    rename_token(ctx, &n->as.fn_param.id);
    rename_type(ctx, &n->as.fn_param.type);
    break;
  case AST_IDENTIF:
    rename_token(ctx, &n->as.identif.val);
    break;
  case AST_CAST:
    rename_type(ctx, &n->as.cast.target);
    break;
  case AST_MEMBER:
    // The member name itself is not mangled, only the base might be
    break;
  default:
    break;
  }
  return VISIT_CONTINUE;
}

void mangle_mod_symbols(Arena *arena, Module *mod) {
  if (!mod || !mod->ast_root)
    return;

  HashMap *rename_map = arena_alloc(arena, sizeof(HashMap));
  map_init(rename_map, arena, 128);

  AstNode *stmt = mod->ast_root->as.block.first_stmt;
  while (stmt) {
    Token *tok = NULL;
    if (stmt->type == AST_FUNC && !stmt->as.func_def.is_extern) {
      tok = &stmt->as.func_def.fn_name;
    } else if ((stmt->type == AST_STRUCT && stmt->as.struct_def.contents) ||
               (stmt->type == AST_UNION && stmt->as.union_def.contents) ||
               stmt->type == AST_ENUM) {
      if (stmt->type == AST_STRUCT)
        tok = &stmt->as.struct_def.structn;
      else if (stmt->type == AST_UNION)
        tok = &stmt->as.union_def.unionn;
      else
        tok = &stmt->as.enum_def.enumn;
    } else if (stmt->type == AST_VAR_DECL) {
      tok = &stmt->as.var_decl.id;
    }

    if (tok && tok->len > 0) {
      size_t new_len = strlen(mod->mod_name) + 1 + tok->len;
      char *new_name = arena_alloc(arena, new_len + 1);
      snprintf(new_name, new_len + 1, "%s_%.*s", mod->mod_name, (int)tok->len,
               tok->start);
      Token *mangled = arena_alloc(arena, sizeof(Token));
      mangled->start = new_name;
      mangled->len = new_len;
      map_set(rename_map, tok->start, tok->len, mangled);
    }
    stmt = stmt->next;
  }

  // Traverse AST and rename all occurrences
  MangleCtx ctx = {.arena = arena, .rename_map = rename_map};
  AstVisitor visitor = {0};
  visitor.user_data = &ctx;
  visitor.enter_node = mangle_enter;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;
  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, mod->ast_root);
  } else {
    fprintf(stderr, "OOM encountered while mangling symbols.\n");
  }

  map_free_buckets(rename_map);
}
