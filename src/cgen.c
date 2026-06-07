#include "arena.h"
#include "c_gen_types.h"
#include "hashmap.h"
#include "sem_types.h"
#include "string_builder.h"
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

void inject_yield_assignments(AstNode *node, const char *var_name,
                              Arena *arena) {
  if (!node)
    return;

  size_t cap = 128;
  AstNode ***stack = malloc(sizeof(AstNode **) * cap);
  size_t top = 0;

  stack[top++] = &node;

  while (top > 0) {
    AstNode **p_n = stack[--top];
    AstNode *n = *p_n;
    if (!n)
      continue;

    if (n->type == AST_BLOCK) {
      AstNode **last = &n->as.block.first_stmt;
      while (*last && (*last)->next) {
        last = &(*last)->next;
      }
      if (*last) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode **) * cap);
        }
        stack[top++] = last;
      }
    } else if (n->type == AST_IF) {
      if (n->as.if_check.action) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode **) * cap);
        }
        stack[top++] = &n->as.if_check.action;
      }
      if (n->as.if_check.elseAct) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode **) * cap);
        }
        stack[top++] = &n->as.if_check.elseAct;
      }
    } else if (n->type == AST_SWITCH) {
      AstNode *c = n->as.switch_stmt.cases;
      while (c) {
        if (c->type == AST_CASE && c->as.case_stmt.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(AstNode **) * cap);
          }
          stack[top++] = &c->as.case_stmt.action;
        }
        c = c->next;
      }
      if (n->as.switch_stmt.default_case) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode **) * cap);
        }
        stack[top++] = &n->as.switch_stmt.default_case;
      }
    } else {
      if (n->type == AST_BREAK || n->type == AST_CONTINUE ||
          n->type == AST_RET || n->type == AST_DEFER ||
          n->type == AST_VAR_DECL) {
        continue;
      }

      AstNode *target = arena_alloc(arena, sizeof(AstNode));
      memset(target, 0, sizeof(AstNode));
      target->type = AST_IDENTIF;

      size_t vlen = strlen(var_name);
      char *vstr = arena_alloc(arena, vlen + 1);
      strcpy(vstr, var_name);

      target->as.identif.val.start = vstr;
      target->as.identif.val.len = vlen;
      target->eval_type = n->eval_type;

      AstNode *assign = arena_alloc(arena, sizeof(AstNode));
      memset(assign, 0, sizeof(AstNode));
      assign->type = AST_BINOP;
      assign->as.binop.op.start = "=";
      assign->as.binop.op.len = 1;
      assign->as.binop.left = target;
      assign->as.binop.right = n;
      assign->eval_type = n->eval_type;

      assign->next = n->next;
      n->next = NULL;

      *p_n = assign;
    }
  }
  free(stack);
}

void generate_c_code(AstNode *root, StringBuilder *sb, HashMap *func_map,
                     Arena *arena, bool is_main_mod) {
  size_t cap = 2048;
  IterFrame *stack = malloc(sizeof(IterFrame) * cap);
  size_t top = 0;

  stack[top++] = (IterFrame){root, 0, NULL, NULL, 0};

  uint64_t yield_blk_ctr = 0;
  while (top > 0) {
    IterFrame *f = &stack[top - 1];
    AstNode *n = f->node;

    if (!n) {
      top--;
      continue;
    }

    // Auto-unwrap single expression blocks
    if (f->step == 0 && (f->flags & 2)) {
      while (n && n->type == AST_BLOCK && n->as.block.first_stmt &&
             !n->as.block.first_stmt->next) {
        AstNode *stmt = n->as.block.first_stmt;
        if (is_c_expr(stmt)) {
          n = stmt;
          f->node = n;
        } else {
          break;
        }
      }
    }

    if (n->type == AST_PROGRAM || n->type == AST_BLOCK ||
        n->type == AST_EXTERN) {
      if (f->step == 0) {
        if (n->type == AST_BLOCK) {
          // Standard C block emission (GNU code removed here)
          sb_append(sb, "{\n");
        }
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

          f->flags = (f->flags & 2) | (needs_semi ? 1 : 0);
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
        if (f->flags & 1)
          sb_append(sb, ";\n");
        f->step = 1;
      } else {
        if (n->type == AST_BLOCK) {
          // Standard C block end (GNU code removed here)
          sb_append(sb, "}\n");
        }
        top--;
      }
    } else if (n->type == AST_FUNC) {
      if (f->step == 0) {
        if (n->as.func_def.fn_name.len == 4 &&
            strncmp(n->as.func_def.fn_name.start, "main", 4) == 0 &&
            is_main_mod) {
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
          if (clean_ret.ptr_depth == 0 && clean_ret.array_dimens == 0) {
            clean_ret.is_mut = true;
          }
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
        DataType decl_type = n->as.var_decl.type;
        // Do not emit extern if the variable is being initialized
        if (n->as.var_decl.init)
          decl_type.is_extern = false;

        gen_type(decl_type, sb);
        sb_append_len(sb, n->as.var_decl.id.start, n->as.var_decl.id.len);
        if (n->as.var_decl.init) {
          sb_append(sb, " = ");
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.var_decl.init, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.binop.left, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.binop.right, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.unop.operand, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.if_check.check, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.while_loop.check, 0, NULL, NULL, 2};
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
          stack[top++] = (IterFrame){n->as.for_loop.check, 0, NULL, NULL, 2};
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
          stack[top++] = (IterFrame){n->as.for_loop.inc, 0, NULL, NULL, 2};
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
            sb_append_len(sb, base->as.identif.val.start,
                          base->as.identif.val.len);
            sb_append(sb, "_");
            sb_append_len(sb, member_node->as.member.name.start,
                          member_node->as.member.name.len);
            sb_append(sb, "(");
            f->aux = n->as.func_call.args;
            f->aux2 = NULL;
            f->step = 2;
          } else {
            Token method = member_node->as.member.name;
            DataType base_eval = base->eval_type;
            Token base_type = base_eval.name;
            if (base_type.len == 0)
              base_type = member_node->as.member.type;

            size_t mangled_len = base_type.len + 1 + method.len;
            char *mangled = arena_alloc(arena, mangled_len + 1);
            memcpy(mangled, base_type.start, base_type.len);
            mangled[base_type.len] = '_';
            memcpy(mangled + base_type.len + 1, method.start, method.len);
            mangled[mangled_len] = '\0';

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

            if (has_self) {
              f->step = 1;
              if (top >= cap) {
                cap *= 2;
                stack = realloc(stack, sizeof(IterFrame) * cap);
                f = &stack[top - 1];
              }
              stack[top++] =
                  (IterFrame){member_node->as.member.base, 0, NULL, NULL, 2};
            } else {
              f->step = 2;
              f->aux = f->aux2;
              f->aux2 = NULL;
            }
          }
        } else if (f->step == 1) {
          if (f->aux2)
            sb_append(sb, ", ");
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
            stack[top++] = (IterFrame){arg, 0, NULL, NULL, 2};
          } else {
            sb_append(sb, ")");
            top--;
          }
        } else if (f->step == 3) {
          if (f->aux)
            sb_append(sb, ", ");
          f->step = 2;
        } else if (f->step == 4) {
          sb_append(sb, ")");
          top--;
        }
      } else {
        if (f->step == 0) {
          bool is_ctor = false;
          Sym *sym = NULL;
          if (n->as.func_call.caller &&
              n->as.func_call.caller->type == AST_IDENTIF) {
            sym = n->as.func_call.caller->as.identif.res_sm;
            if (sym && (sym->kind == SYM_STRUCT || sym->kind == SYM_UNION ||
                        sym->kind == SYM_ENUM)) {
              is_ctor = true;
            }
          }
          f->aux2 = (AstNode *)(uintptr_t)(is_ctor ? 1 : 0);
          if (is_ctor) {
            switch (sym->kind) {
            case SYM_STRUCT:
              sb_append(sb, "(struct ");
              break;
            case SYM_UNION:
              sb_append(sb, "(union ");
              break;
            case SYM_ENUM:
              sb_append(sb, "(enum ");
              break;
            default:
              sb_append(sb, "(");
              break;
            }
          }

          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){n->as.func_call.caller, 0, NULL, NULL, 2};
        } else if (f->step == 1) {
          bool is_ctor = (uintptr_t)f->aux2;
          if (is_ctor)
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
            stack[top++] = (IterFrame){arg, 0, NULL, NULL, 2};
          } else {
            bool is_ctor = (uintptr_t)f->aux2;
            sb_append(sb, is_ctor ? "}" : ")");
            top--;
          }
        } else if (f->step == 3) {
          sb_append(sb, ", ");
          f->step = 2;
        } else if (f->step == 4) {
          bool is_ctor = (uintptr_t)f->aux2;
          sb_append(sb, is_ctor ? "}" : ")");
          top--;
        }
      }
    } else if (n->type == AST_ARRAY_LIT) {
      if (f->step == 0) {
        DataType t = n->eval_type;
        if (t.name.len > 0) {
          sb_append(sb, "(");

          int array_lit_depth = 0;
          for (long i = (long)top - 2; i >= 0; i--) {
            if (stack[i].node && stack[i].node->type == AST_ARRAY_LIT) {
              array_lit_depth++;
            } else {
              break;
            }
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
          stack[top++] = (IterFrame){elem, 0, NULL, NULL, 2};
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
            f->step = 1;
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){unwrapped, 0, NULL, NULL, 2};
          } else {
            bool is_void =
                (block->eval_type.name.len == 4 &&
                 strncmp(block->eval_type.name.start, "void", 4) == 0 &&
                 block->eval_type.ptr_depth == 0 &&
                 block->eval_type.array_dimens == 0) ||
                (block->eval_type.name.len == 0);

            char var_name[64];
            sprintf(var_name, "_tx_blk_%zu", ++yield_blk_ctr);

            if (!is_void) {
              DataType mut_type = block->eval_type;
              mut_type.is_mut = true;
              mut_type.is_threadlocal = false;
              mut_type.is_static = false;
              mut_type.is_extern = false;
              gen_type(mut_type, sb);
              sb_append(sb, var_name);
              sb_append(sb, ";\n");
              inject_yield_assignments(block, var_name, arena);
            }

            f->step = 3;
            f->aux = block->as.block.first_stmt;
            f->aux2 = (AstNode *)(uintptr_t)is_void;
          }
        } else {
          sb_append(sb, "return ");
          if (n->as.ret_stmt.expr) {
            f->step = 1;
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(IterFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (IterFrame){n->as.ret_stmt.expr, 0, NULL, NULL, 2};
          } else {
            sb_append(sb, ";\n");
            top--;
          }
        }
      } else if (f->step == 1) {
        sb_append(sb, ";\n");
        top--;
      } else if (f->step == 3) {
        if (f->aux) {
          AstNode *stmt = f->aux;
          f->aux = f->aux->next;

          bool needs_semi =
              (stmt->type == AST_FUNC_CALL || stmt->type == AST_BINOP ||
               stmt->type == AST_UOP || stmt->type == AST_IDENTIF ||
               stmt->type == AST_NUM_LIT || stmt->type == AST_STR_LIT ||
               stmt->type == AST_MEMBER);

          f->flags = needs_semi ? 1 : 0;
          f->step = 4;

          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (IterFrame){stmt, 0, NULL, NULL, 0};
        } else {
          bool is_void = (bool)(uintptr_t)f->aux2;
          if (!is_void) {
            char var_name[64];
            sprintf(var_name, "_tx_blk_%zu", yield_blk_ctr);
            sb_append(sb, "return ");
            sb_append(sb, var_name);
            sb_append(sb, ";\n");
          } else {
            sb_append(sb, "return;\n");
          }
          top--;
        }
      } else if (f->step == 4) {
        if (f->flags & 1)
          sb_append(sb, ";\n");
        f->step = 3;
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
        stack[top++] = (IterFrame){n->as.cast.op, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.member.base, 0, NULL, NULL, 2};
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
        stack[top++] = (IterFrame){n->as.index.base, 0, NULL, NULL, 2};
      } else if (f->step == 1) {
        sb_append(sb, "[");
        f->step = 2;
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(IterFrame) * cap);
          f = &stack[top - 1];
        }
        stack[top++] = (IterFrame){n->as.index.index, 0, NULL, NULL, 2};
      } else {
        sb_append(sb, "]");
        top--;
      }
    } else if (n->type == AST_STRUCT || n->type == AST_UNION ||
               n->type == AST_ENUM) {
      bool is_enum = (n->type == AST_ENUM);
      bool is_nested = n->is_nested_sue;

      bool is_opaque = (n->type == AST_STRUCT && !n->as.struct_def.contents) ||
                       (n->type == AST_UNION && !n->as.union_def.contents);

      if (f->step == 0) {
        if (is_nested) {
          sb_append(sb, is_enum                   ? "enum {\n"
                        : (n->type == AST_STRUCT) ? "struct {\n"
                                                  : "union {\n");
        } else {
          Token tag = is_enum                   ? n->as.enum_def.enumn
                      : (n->type == AST_STRUCT) ? n->as.struct_def.structn
                                                : n->as.union_def.unionn;

          if (is_opaque && !is_enum) {
            sb_append(sb, "typedef ");
            sb_append(sb, (n->type == AST_STRUCT) ? "struct " : "union ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, " ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, ";\n");
            top--;
            continue;
          }

          if (is_enum) {
            sb_append(sb, "typedef enum ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, " {\n");
          } else {
            sb_append(sb, "typedef ");
            sb_append(sb, (n->type == AST_STRUCT) ? "struct " : "union ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, " ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, ";\n");
            sb_append(sb, (n->type == AST_STRUCT) ? "struct " : "union ");
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, " {\n");
          }
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
          if (is_enum) {
            sb_append(sb, "} ");
            Token tag = n->as.enum_def.enumn;
            sb_append_len(sb, tag.start, tag.len);
            sb_append(sb, ";\n");
          } else {
            sb_append(sb, "};\n");
          }
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
          stack[top++] = (IterFrame){n->as.enum_member.val, 0, NULL, NULL, 2};
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
    } else if (n->type == AST_SIZEOF) {
      if (f->step == 0) {
        sb_append(sb, "sizeof(");
        if (n->as.sizeof_expr.is_type) {
          gen_type(n->as.sizeof_expr.target_type, sb);
          sb_append(sb, ")");
          top--;
        } else {
          f->step = 1;
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(IterFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] =
              (IterFrame){n->as.sizeof_expr.target_expr, 0, NULL, NULL, 2};
        }
      } else if (f->step == 1) {
        sb_append(sb, ")");
        top--;
      }
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
      sue = (n->type == AST_STRUCT)  ? n->as.struct_def.structn
            : (n->type == AST_UNION) ? n->as.union_def.unionn
                                     : n->as.enum_def.enumn;

      if (frame.sue.len > 0)
        n->is_nested_sue = true;

      AstNode **prev_ptr = (n->type == AST_STRUCT)  ? &n->as.struct_def.contents
                           : (n->type == AST_UNION) ? &n->as.union_def.contents
                                                    : &n->as.enum_def.contents;

      while (*prev_ptr) {
        AstNode *child = *prev_ptr;
        if (child->type == AST_FUNC) {
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

          *prev_ptr = child->next;
          child->next = NULL;

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
        if (!n->as.identif.res_sm) {
          n->as.identif.res_sm = arena_alloc(arena, sizeof(Sym));
        }
        n->as.identif.res_sm->kind = SYM_STRUCT;
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
            (n->as.fn_param.id.start == NULL ||
             strncmp(n->as.fn_param.id.start, "self", 4) == 0) &&
            sue.len > 0) {
          n->as.fn_param.id = sue;
          n->as.fn_param.type.name = sue;
        }
        break;
      case AST_VAR_DECL:
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
      case AST_MEMBER: {
        AstNode *base = n->as.member.base;

        if (base && base->type == AST_IDENTIF && base->as.identif.res_sm &&
            base->as.identif.res_sm->is_imported_mod) {

          Token mod_name = base->as.identif.val;
          Token mem_name = n->as.member.name;

          size_t new_len = mod_name.len + 1 + mem_name.len;
          char *new_name = arena_alloc(arena, new_len + 1);
          sprintf(new_name, "%.*s_%.*s", mod_name.len, mod_name.start,
                  mem_name.len, mem_name.start);

          n->type = AST_IDENTIF;
          n->as.identif.val.start = new_name;
          n->as.identif.val.len = new_len;
          n->as.identif.res_sm = NULL;

          break;
        }

        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(FlattenFrame) * cap);
        }
        stack[top++] = (FlattenFrame){n->as.member.base, sue};
        break;
      }
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

// Macro to safely clone and push child nodes without recursion
#define CLONE_CHILD(child_ptr)                                                 \
  if (src->child_ptr) {                                                        \
    dst->child_ptr = arena_alloc(arena, sizeof(AstNode));                      \
    *dst->child_ptr = *src->child_ptr;                                         \
    dst->child_ptr->next = NULL;                                               \
    if (top >= cap) {                                                          \
      cap *= 2;                                                                \
      stack = realloc(stack, sizeof(ClonePair) * cap);                         \
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

void lower_defers(AstNode *root, Arena *arena) {
  if (!root)
    return;

  size_t cap = 1024;
  LowerFrame *stack = malloc(sizeof(LowerFrame) * cap);
  size_t top = 0;

  AstNode **defers = malloc(sizeof(AstNode *) * 1024);
  size_t defer_count = 0;

  stack[top++] = (LowerFrame){root, 0, 0, 0, 0};

  uint64_t rid_c = 0;
  while (top > 0) {
    LowerFrame *f = &stack[top - 1];
    AstNode *n = f->node;

    if (n->type == AST_FUNC) {
      if (f->step == 0) {
        f->func_base = defer_count;
        f->step = 1;
        if (n->as.func_def.block) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (LowerFrame){n->as.func_def.block, 0, f->func_base,
                                      f->loop_base, defer_count};
        }
      } else {
        top--;
      }
    } else if (n->type == AST_WHILE || n->type == AST_FOR) {
      if (f->step == 0) {
        f->loop_base = defer_count;
        f->step = 1;
        AstNode *body = (n->type == AST_WHILE) ? n->as.while_loop.action
                                               : n->as.for_loop.action;
        if (body) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] =
              (LowerFrame){body, 0, f->func_base, f->loop_base, defer_count};
        }
      } else {
        top--;
      }
    } else if (n->type == AST_BLOCK) {
      if (f->step == 0) {
        f->block_base = defer_count;

        AstNode **ptr = &n->as.block.first_stmt;
        while (*ptr) {
          if ((*ptr)->type == AST_DEFER) {
            defers[defer_count++] = *ptr;
            *ptr = (*ptr)->next;
          } else {
            ptr = &(*ptr)->next;
          }
        }

        f->step = 1;

        AstNode *stmt = n->as.block.first_stmt;
        size_t count = 0;
        while (stmt) {
          count++;
          stmt = stmt->next;
        }
        if (count > 0) {
          AstNode **arr = malloc(sizeof(AstNode *) * count);
          stmt = n->as.block.first_stmt;
          for (size_t i = 0; i < count; i++) {
            arr[i] = stmt;
            stmt = stmt->next;
          }
          for (int i = count - 1; i >= 0; i--) {
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(LowerFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (LowerFrame){arr[i], 0, f->func_base, f->loop_base,
                                        defer_count};
          }
          free(arr);
        }
      } else if (f->step == 1) {
        AstNode *tail = n->as.block.first_stmt;
        while (tail && tail->next)
          tail = tail->next;

        // Inject block-specific defers at the very end of the block
        for (size_t i = defer_count; i > f->block_base; i--) {
          AstNode *cloned =
              clone_ast(defers[i - 1]->as.defer_stmt.contents, arena);
          if (!n->as.block.first_stmt)
            n->as.block.first_stmt = cloned;
          else
            tail->next = cloned;
          while (tail->next)
            tail = tail->next;
        }
        defer_count = f->block_base;
        top--;
      }
    } else if (n->type == AST_RET) {
      if (defer_count > f->func_base) {
        AstNode *blk = arena_alloc(arena, sizeof(AstNode));
        memset(blk, 0, sizeof(AstNode));
        blk->type = AST_BLOCK;

        AstNode *ret_val_decl = NULL;
        AstNode *ret_val_ident = NULL;

        if (n->as.ret_stmt.expr) {
          ret_val_decl = arena_alloc(arena, sizeof(AstNode));
          memset(ret_val_decl, 0, sizeof(AstNode));
          ret_val_decl->type = AST_VAR_DECL;
          ret_val_decl->as.var_decl.type = n->as.ret_stmt.expr->eval_type;
          ret_val_decl->as.var_decl.type.is_threadlocal = false;
          ret_val_decl->as.var_decl.type.is_static = false;
          ret_val_decl->as.var_decl.type.is_extern = false;

          char *tmp_name = arena_alloc(arena, 64);
          sprintf(tmp_name, "_tx_ret_%zu", ++rid_c);
          ret_val_decl->as.var_decl.id = (Token){
              tmp_name, (unsigned int)strlen(tmp_name), TOKEN_IDENTIF, 0, 0};
          ret_val_decl->as.var_decl.init = n->as.ret_stmt.expr;

          ret_val_ident = arena_alloc(arena, sizeof(AstNode));
          memset(ret_val_ident, 0, sizeof(AstNode));
          ret_val_ident->type = AST_IDENTIF;
          ret_val_ident->as.identif.val = ret_val_decl->as.var_decl.id;
        }

        AstNode *tail = NULL;
        if (ret_val_decl) {
          blk->as.block.first_stmt = ret_val_decl;
          tail = ret_val_decl;
        }

        for (size_t i = defer_count; i > f->func_base; i--) {
          AstNode *cloned =
              clone_ast(defers[i - 1]->as.defer_stmt.contents, arena);
          if (!blk->as.block.first_stmt)
            blk->as.block.first_stmt = cloned;
          else
            tail->next = cloned;
          while (tail->next)
            tail = tail->next;
        }

        AstNode *new_ret = arena_alloc(arena, sizeof(AstNode));
        memset(new_ret, 0, sizeof(AstNode));
        new_ret->type = AST_RET;
        new_ret->as.ret_stmt.expr = ret_val_ident;

        if (!blk->as.block.first_stmt)
          blk->as.block.first_stmt = new_ret;
        else
          tail->next = new_ret;

        AstNode *nxt = n->next;
        *n = *blk;
        n->next = nxt;
      }
      top--;
    } else if (n->type == AST_BREAK || n->type == AST_CONTINUE) {
      if (defer_count > f->loop_base) {
        AstNode *blk = arena_alloc(arena, sizeof(AstNode));
        memset(blk, 0, sizeof(AstNode));
        blk->type = AST_BLOCK;
        AstNode *tail = NULL;

        for (size_t i = defer_count; i > f->loop_base; i--) {
          AstNode *cloned =
              clone_ast(defers[i - 1]->as.defer_stmt.contents, arena);
          if (!blk->as.block.first_stmt)
            blk->as.block.first_stmt = cloned;
          else
            tail->next = cloned;
          while (tail->next)
            tail = tail->next;
        }

        AstNode *new_ctrl = arena_alloc(arena, sizeof(AstNode));
        memset(new_ctrl, 0, sizeof(AstNode));
        new_ctrl->type = n->type;
        if (n->type == AST_BREAK)
          new_ctrl->as.break_stmt = n->as.break_stmt;
        else
          new_ctrl->as.continue_stmt = n->as.continue_stmt;

        if (!blk->as.block.first_stmt)
          blk->as.block.first_stmt = new_ctrl;
        else
          tail->next = new_ctrl;

        AstNode *nxt = n->next;
        *n = *blk;
        n->next = nxt;
      }
      top--;
    } else if (n->type == AST_IF) {
      if (f->step == 0) {
        f->step = 1;
        if (n->as.if_check.elseAct) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (LowerFrame){n->as.if_check.elseAct, 0, f->func_base,
                                      f->loop_base, f->block_base};
        }
        if (n->as.if_check.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (LowerFrame){n->as.if_check.action, 0, f->func_base,
                                      f->loop_base, f->block_base};
        }
      } else {
        top--;
      }
    } else if (n->type == AST_SWITCH) {
      if (f->step == 0) {
        f->step = 1;
        if (n->as.switch_stmt.default_case) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] =
              (LowerFrame){n->as.switch_stmt.default_case, 0, f->func_base,
                           f->loop_base, f->block_base};
        }
        AstNode *c = n->as.switch_stmt.cases;
        while (c) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] =
              (LowerFrame){c, 0, f->func_base, f->loop_base, f->block_base};
          c = c->next;
        }
      } else {
        top--;
      }
    } else if (n->type == AST_CASE) {
      if (f->step == 0) {
        f->step = 1;
        if (n->as.case_stmt.action) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(LowerFrame) * cap);
            f = &stack[top - 1];
          }
          stack[top++] = (LowerFrame){n->as.case_stmt.action, 0, f->func_base,
                                      f->loop_base, f->block_base};
        }
      } else {
        top--;
      }
    } else if (n->type == AST_PROGRAM || n->type == AST_EXTERN) {
      if (f->step == 0) {
        f->step = 1;
        AstNode *stmt = (n->type == AST_PROGRAM) ? n->as.block.first_stmt
                                                 : n->as.extern_block.contents;
        size_t count = 0;
        while (stmt) {
          count++;
          stmt = stmt->next;
        }
        if (count > 0) {
          AstNode **arr = malloc(sizeof(AstNode *) * count);
          stmt = (n->type == AST_PROGRAM) ? n->as.block.first_stmt
                                          : n->as.extern_block.contents;
          for (size_t i = 0; i < count; i++) {
            arr[i] = stmt;
            stmt = stmt->next;
          }
          for (int i = count - 1; i >= 0; i--) {
            if (top >= cap) {
              cap *= 2;
              stack = realloc(stack, sizeof(LowerFrame) * cap);
              f = &stack[top - 1];
            }
            stack[top++] = (LowerFrame){arr[i], 0, f->func_base, f->loop_base,
                                        f->block_base};
          }
          free(arr);
        }
      } else {
        top--;
      }
    } else {
      top--;
    }
  }
  free(stack);
  free(defers);
}

bool output_to_c_and_compile(SemCtx *sem, const char *out_binary_name,
                             const char *compiler, const char **flags,
                             int flag_count, Arena *arena, Module *main_mod) {
  if (!sem)
    return false;

  StringBuilder code;
  sb_init(&code);

  // Standard libs
  sb_append(&code, "/* Auto-generated by Tereix Transpiler */\n");
  sb_append(&code, "#include <stdio.h>\n");
  sb_append(&code, "#include <stdlib.h>\n");
  sb_append(&code, "#include <stdint.h>\n");
  sb_append(&code, "#include <stdbool.h>\n");
  sb_append(&code, "#include <string.h>\n\n");

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

      entry = entry->next; // Move to the next entry in the chain
    }
  }

  const char *tmp_c_file = "output_gen.c";
  bool requires_compilation = true;

  // Verify the compiled binary hasn't been deleted
  FILE *bin_test = fopen(out_binary_name, "rb");
  if (bin_test) {
    fclose(bin_test);

    // Read the existing generated C file
    FILE *old_c = fopen(tmp_c_file, "rb");
    if (old_c) {
      fseek(old_c, 0, SEEK_END);
      long old_size = ftell(old_c);
      fseek(old_c, 0, SEEK_SET);

      // Compare sizes, then contents
      if (old_size == (long)code.len) {
        char *old_buf = malloc(old_size);
        if (fread(old_buf, 1, old_size, old_c) == (size_t)old_size) {
          if (memcmp(old_buf, code.buf, old_size) == 0) {
            requires_compilation = false;
          }
        }
        free(old_buf);
      }
      fclose(old_c);
    }
  }

  // Early exit if the C output is identical and binary exists
  if (!requires_compilation) {
    printf("Generated C code unchanged. Skipping GCC compilation.\n");
    sb_free(&code);
    map_free_buckets(global_func_map);
    return true;
  }

  // Proceed to overwrite and invoke GCC
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

  sb_append(&cmd, compiler);
  sb_append(&cmd, " -o ");
  sb_append(&cmd, out_binary_name);
  sb_append(&cmd, " ");
  sb_append(&cmd, tmp_c_file);

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
    map_free_buckets(global_func_map);
    return false;
  }

  char *token = strtok(cmd_copy, " ");
  while (token) {
    argv = realloc(argv, (argc + 2) * sizeof(char *));
    argv[argc++] = token;
    token = strtok(NULL, " ");
  }
  argv[argc] = NULL;

  pid_t pid = fork();
  int res = -1;

  if (pid == -1) {
    perror("fork");
    free(cmd_copy);
    free(argv);
    sb_free(&cmd);
    map_free_buckets(global_func_map);
    return false;
  } else if (pid == 0) {
    // Child process execute the compiler
    execvp(argv[0], argv);
    // If we get here exec failed
    perror("execvp");
    _exit(127);
  } else {
    // Parent process wait for child
    int status;
    if (waitpid(pid, &status, 0) == -1) {
      perror("waitpid");
      res = -1;
    } else if (WIFEXITED(status)) {
      res = WEXITSTATUS(status);
    } else {
      res = -1;
    }
  }

  free(cmd_copy);
  free(argv);

  sb_free(&cmd);
  map_free_buckets(global_func_map);

  return res == 0;
}

void mangle_mod_symbols(Arena *arena, Module *mod) {
  HashMap *rename_map = arena_alloc(arena, sizeof(HashMap));
  map_init(rename_map, arena, 128);

  AstNode *stmt = mod->ast_root->as.block.first_stmt;
  while (stmt) {
    Token *old_tok = NULL;

    if (stmt->type == AST_FUNC && !stmt->as.func_def.is_extern) {
      old_tok = &stmt->as.func_def.fn_name;
    } else if ((stmt->type == AST_STRUCT && stmt->as.struct_def.contents) ||
               (stmt->type == AST_UNION && stmt->as.union_def.contents) ||
               stmt->type == AST_ENUM) {
      old_tok = (stmt->type == AST_STRUCT)  ? &stmt->as.struct_def.structn
                : (stmt->type == AST_UNION) ? &stmt->as.union_def.unionn
                                            : &stmt->as.enum_def.enumn;
    } else if (stmt->type == AST_VAR_DECL) {
      old_tok = &stmt->as.var_decl.id;
    }

    if (old_tok && old_tok->len > 0) {
      size_t nlen = strlen(mod->mod_name) + 1 + old_tok->len;
      char *new_name = arena_alloc(arena, nlen + 1);
      sprintf(new_name, "%s_%.*s", mod->mod_name, (int)old_tok->len,
              old_tok->start);

      Token *mangled = arena_alloc(arena, sizeof(Token));
      mangled->start = new_name;
      mangled->len = nlen;

      map_set(rename_map, old_tok->start, old_tok->len, mangled);
    }
    stmt = stmt->next;
  }

  size_t cap = 1024;
  AstNode **stack = malloc(sizeof(AstNode *) * cap);
  size_t top = 0;

  stack[top++] = mod->ast_root;

#define RENAME_TOK(tok)                                                        \
  do {                                                                         \
    if ((tok).len > 0) {                                                       \
      Token *rep = map_get(rename_map, (tok).start, (tok).len);                \
      if (rep) {                                                               \
        (tok) = *rep;                                                          \
      }                                                                        \
    }                                                                          \
  } while (0)

  while (top > 0) {
    AstNode *n = stack[--top];
    if (!n)
      continue;

    RENAME_TOK(n->eval_type.name);

    switch (n->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
    case AST_EXTERN: {
      AstNode *curr = (n->type == AST_EXTERN) ? n->as.extern_block.contents
                                              : n->as.block.first_stmt;
      while (curr) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = curr;
        curr = curr->next;
      }
      break;
    }
    case AST_FUNC:
      RENAME_TOK(n->as.func_def.fn_name);
      RENAME_TOK(n->as.func_def.ret_type.name);
      if (n->as.func_def.params) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.func_def.params;
      }
      if (n->as.func_def.block) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.func_def.block;
      }
      break;
    case AST_PARAM:
      RENAME_TOK(n->as.fn_param.id);
      RENAME_TOK(n->as.fn_param.type.name);
      break;
    case AST_VAR_DECL:
      RENAME_TOK(n->as.var_decl.id);
      RENAME_TOK(n->as.var_decl.type.name);
      if (n->as.var_decl.init) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.var_decl.init;
      }
      break;
    case AST_STRUCT:
    case AST_UNION:
    case AST_ENUM: {
      Token *tag = (n->type == AST_STRUCT)  ? &n->as.struct_def.structn
                   : (n->type == AST_UNION) ? &n->as.union_def.unionn
                                            : &n->as.enum_def.enumn;
      RENAME_TOK(*tag);
      AstNode *child = (n->type == AST_STRUCT)  ? n->as.struct_def.contents
                       : (n->type == AST_UNION) ? n->as.union_def.contents
                                                : n->as.enum_def.contents;
      while (child) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = child;
        child = child->next;
      }
      break;
    }
    case AST_ENUM_MEMBER:
      RENAME_TOK(n->as.enum_member.name);
      if (n->as.enum_member.val) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.enum_member.val;
      }
      break;
    case AST_IDENTIF:
      RENAME_TOK(n->as.identif.val);
      break;
    case AST_CAST:
      RENAME_TOK(n->as.cast.target.name);
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.cast.op;
      break;
    case AST_FUNC_CALL: {
      AstNode *arg = n->as.func_call.args;
      while (arg) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = arg;
        arg = arg->next;
      }
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.func_call.caller;
      break;
    }
    case AST_BINOP:
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.binop.right;
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.binop.left;
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.unop.operand;
      break;
    case AST_IF:
      if (n->as.if_check.elseAct) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.if_check.elseAct;
      }
      if (n->as.if_check.action) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.if_check.action;
      }
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.if_check.check;
      break;
    case AST_WHILE:
      if (n->as.while_loop.action) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.while_loop.action;
      }
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.while_loop.check;
      break;
    case AST_FOR:
      if (n->as.for_loop.action) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.for_loop.action;
      }
      if (n->as.for_loop.inc) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.for_loop.inc;
      }
      if (n->as.for_loop.check) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.for_loop.check;
      }
      if (n->as.for_loop.init) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.for_loop.init;
      }
      break;
    case AST_MEMBER:
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.member.base;
      break;
    case AST_INDEX:
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.index.index;
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.index.base;
      break;
    case AST_ARRAY_LIT: {
      AstNode *elem = n->as.array_lit.elements;
      while (elem) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = elem;
        elem = elem->next;
      }
      break;
    }
    case AST_RET:
      if (n->as.ret_stmt.expr) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.ret_stmt.expr;
      }
      break;
    case AST_DEFER:
      if (n->as.defer_stmt.contents) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.defer_stmt.contents;
      }
      break;
    case AST_SWITCH:
      if (n->as.switch_stmt.default_case) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.switch_stmt.default_case;
      }
      {
        AstNode *c = n->as.switch_stmt.cases;
        while (c) {
          if (top >= cap) {
            cap *= 2;
            stack = realloc(stack, sizeof(AstNode *) * cap);
          }
          stack[top++] = c;
          c = c->next;
        }
      }
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.switch_stmt.check;
      break;
    case AST_CASE:
      if (n->as.case_stmt.action) {
        if (top >= cap) {
          cap *= 2;
          stack = realloc(stack, sizeof(AstNode *) * cap);
        }
        stack[top++] = n->as.case_stmt.action;
      }
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = n->as.case_stmt.val;
      break;
    default:
      break;
    }
  }

#undef RENAME_TOK
  free(stack);
  map_free_buckets(rename_map);
}
