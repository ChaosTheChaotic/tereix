#include "ast_visitor.h"
#include "parse_types.h"
#include <stdlib.h>

#define STATE_ENTER 0
#define STATE_EXIT 1
#define STATE_INTERLEAVE_BASE 2

typedef struct {
  AstNode *node;
  int state;
} VisitorFrame;

bool ast_traverse(AstVisitor *visitor, AstNode *root) {
  if (!root)
    return true;

  size_t cap = 1024;
  VisitorFrame *stack = malloc(sizeof(VisitorFrame) * cap);
  if (!stack) {
    if (visitor->panic_env)
      longjmp(*visitor->panic_env, ERR_OOM);
    return false;
  }

  size_t top = 0;
  stack[top++] = (VisitorFrame){root, STATE_ENTER};

  while (top > 0) {
    VisitorFrame frame = stack[--top];
    AstNode *n = frame.node;

    if (frame.state == STATE_EXIT) {
      if (visitor->exit_node)
        visitor->exit_node(visitor, n);
      continue;
    }

    if (frame.state >= STATE_INTERLEAVE_BASE) {
      if (visitor->interleave_node) {
        visitor->interleave_node(visitor, n,
                                 frame.state - STATE_INTERLEAVE_BASE);
      }
      continue;
    }

    VisitResult res = VISIT_CONTINUE;
    if (visitor->enter_node) {
      res = visitor->enter_node(visitor, n);
    }

    if (res == VISIT_ABORT) {
      free(stack);
      return false;
    }

    if (res == VISIT_SKIP_CHILDREN) {
      if (visitor->exit_node) {
        visitor->exit_node(visitor, n);
      }
      continue;
    }

    if (visitor->exit_node) {
      if (top >= cap) {
        size_t new_cap = cap * 2;
        VisitorFrame *new_stack =
            realloc(stack, sizeof(VisitorFrame) * new_cap);
        if (!new_stack) {
          free(stack);
          if (visitor->panic_env)
            longjmp(*visitor->panic_env, ERR_OOM);
          return false;
        }
        stack = new_stack;
        cap = new_cap;
      }
      stack[top++] = (VisitorFrame){n, STATE_EXIT};
    }

#define PUSH_NODE(child)                                                       \
  do {                                                                         \
    if ((child)) {                                                             \
      if (top >= cap) {                                                        \
        size_t new_cap = cap * 2;                                              \
        VisitorFrame *new_stack =                                              \
            realloc(stack, sizeof(VisitorFrame) * new_cap);                    \
        if (!new_stack) {                                                      \
          free(stack);                                                         \
          if (visitor->panic_env)                                              \
            longjmp(*visitor->panic_env, ERR_OOM);                             \
          return false;                                                        \
        }                                                                      \
        stack = new_stack;                                                     \
        cap = new_cap;                                                         \
      }                                                                        \
      stack[top++] = (VisitorFrame){(child), STATE_ENTER};                     \
    }                                                                          \
  } while (0)

#define PUSH_INTERLEAVE(step)                                                  \
  do {                                                                         \
    if (visitor->interleave_node) {                                            \
      if (top >= cap) {                                                        \
        size_t new_cap = cap * 2;                                              \
        VisitorFrame *new_stack =                                              \
            realloc(stack, sizeof(VisitorFrame) * new_cap);                    \
        if (!new_stack) {                                                      \
          free(stack);                                                         \
          if (visitor->panic_env)                                              \
            longjmp(*visitor->panic_env, ERR_OOM);                             \
          return false;                                                        \
        }                                                                      \
        stack = new_stack;                                                     \
        cap = new_cap;                                                         \
      }                                                                        \
      stack[top++] = (VisitorFrame){n, STATE_INTERLEAVE_BASE + (step)};        \
    }                                                                          \
  } while (0)

// Reverses linked lists so they execute left to right top to bottom
#define PUSH_LIST(head)                                                        \
  do {                                                                         \
    AstNode *_curr = (head);                                                   \
    size_t _cnt = 0;                                                           \
    while (_curr) {                                                            \
      _cnt++;                                                                  \
      _curr = _curr->next;                                                     \
    }                                                                          \
    if (_cnt > 0) {                                                            \
      AstNode **_arr = malloc(sizeof(AstNode *) * _cnt);                       \
      if (!_arr) {                                                             \
        free(stack);                                                           \
        if (visitor->panic_env)                                                \
          longjmp(*visitor->panic_env, ERR_OOM);                               \
        return false;                                                          \
      }                                                                        \
      _curr = (head);                                                          \
      for (size_t _i = 0; _i < _cnt; _i++) {                                   \
        _arr[_i] = _curr;                                                      \
        _curr = _curr->next;                                                   \
      }                                                                        \
      for (int _i = (int)_cnt - 1; _i >= 0; _i--) {                            \
        PUSH_NODE(_arr[_i]);                                                   \
      }                                                                        \
      free(_arr);                                                              \
    }                                                                          \
  } while (0)

#define PUSH_LIST_INTERLEAVED(head, base_step)                                 \
  do {                                                                         \
    AstNode *_curr = (head);                                                   \
    size_t _cnt = 0;                                                           \
    while (_curr) {                                                            \
      _cnt++;                                                                  \
      _curr = _curr->next;                                                     \
    }                                                                          \
    if (_cnt > 0) {                                                            \
      AstNode **_arr = malloc(sizeof(AstNode *) * _cnt);                       \
      if (!_arr) {                                                             \
        free(stack);                                                           \
        if (visitor->panic_env)                                                \
          longjmp(*visitor->panic_env, ERR_OOM);                               \
        return false;                                                          \
      }                                                                        \
      _curr = (head);                                                          \
      for (size_t _i = 0; _i < _cnt; _i++) {                                   \
        _arr[_i] = _curr;                                                      \
        _curr = _curr->next;                                                   \
      }                                                                        \
      for (int _i = (int)_cnt - 1; _i >= 0; _i--) {                            \
        PUSH_NODE(_arr[_i]);                                                   \
        if (_i > 0)                                                            \
          PUSH_INTERLEAVE((base_step) + _i - 1);                               \
      }                                                                        \
      free(_arr);                                                              \
    }                                                                          \
  } while (0)

    // Dispatch children based on node type
    switch (n->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
      PUSH_LIST_INTERLEAVED(n->as.block.first_stmt, 0);
      break;
    case AST_FUNC:
      PUSH_NODE(n->as.func_def.block);
      PUSH_LIST_INTERLEAVED(n->as.func_def.params, 0);
      break;
    case AST_VAR_DECL:
      PUSH_NODE(n->as.var_decl.init);
      break;
    case AST_BINOP:
      PUSH_NODE(n->as.binop.right);
      PUSH_INTERLEAVE(0);
      PUSH_NODE(n->as.binop.left);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PUSH_NODE(n->as.unop.operand);
      break;
    case AST_IF:
      PUSH_NODE(n->as.if_check.elseAct);
      if (n->as.if_check.elseAct)
        PUSH_INTERLEAVE(1);
      PUSH_NODE(n->as.if_check.action);
      PUSH_INTERLEAVE(0);
      PUSH_NODE(n->as.if_check.check);
      break;
    case AST_WHILE:
      PUSH_NODE(n->as.while_loop.action);
      PUSH_NODE(n->as.while_loop.check);
      break;
    case AST_FOR:
      PUSH_NODE(n->as.for_loop.action);
      PUSH_NODE(n->as.for_loop.inc);
      PUSH_NODE(n->as.for_loop.check);
      PUSH_NODE(n->as.for_loop.init);
      break;
    case AST_FUNC_CALL:
      PUSH_LIST_INTERLEAVED(n->as.func_call.args, 0);
      PUSH_INTERLEAVE(0);
      PUSH_NODE(n->as.func_call.caller);
      break;
    case AST_ARRAY_LIT:
      PUSH_LIST_INTERLEAVED(n->as.array_lit.elements, 0);
      break;
    case AST_INDEX:
      PUSH_NODE(n->as.index.index);
      PUSH_INTERLEAVE(0);
      PUSH_NODE(n->as.index.base);
      break;
    case AST_MEMBER:
      PUSH_NODE(n->as.member.base);
      break;
    case AST_STRUCT:
      PUSH_LIST_INTERLEAVED(n->as.struct_def.contents, 0);
      break;
    case AST_UNION:
      PUSH_LIST_INTERLEAVED(n->as.union_def.contents, 0);
      break;
    case AST_ENUM:
      PUSH_LIST_INTERLEAVED(n->as.enum_def.contents, 0);
      break;
    case AST_ENUM_MEMBER:
      PUSH_NODE(n->as.enum_member.val);
      break;
    case AST_DEFER:
      PUSH_NODE(n->as.defer_stmt.contents);
      break;
    case AST_EXTERN:
      PUSH_LIST_INTERLEAVED(n->as.extern_block.contents, 0);
      break;
    case AST_SWITCH:
      PUSH_NODE(n->as.switch_stmt.default_case);
      PUSH_LIST_INTERLEAVED(n->as.switch_stmt.cases, 0);
      PUSH_NODE(n->as.switch_stmt.check);
      break;
    case AST_CASE:
      PUSH_NODE(n->as.case_stmt.action);
      PUSH_NODE(n->as.case_stmt.val);
      break;
    case AST_CAST:
      PUSH_NODE(n->as.cast.op);
      break;
    case AST_RET:
      PUSH_NODE(n->as.ret_stmt.expr);
      break;
    case AST_SIZEOF:
      if (!n->as.sizeof_expr.is_type)
        PUSH_NODE(n->as.sizeof_expr.target_expr);
      break;
    default:
      break;
    }

#undef PUSH_NODE
#undef PUSH_LIST
  }

  free(stack);
  return true;
}
