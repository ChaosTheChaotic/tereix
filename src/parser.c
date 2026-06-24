#include "ast_types.h"
#include "parse_types.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

inline AstNode *new_node_with_src(Arena *arena, ASTN_TYPE type, ParseCtx *ctx) {
    AstNode *n = new_node(arena, type);
    if (n) {
        n->src_start = ctx->curr.start;
    }
    return n;
}

#undef new_node
#define new_node(arena, type) new_node_with_src(arena, type, ctx)

void report_error(ParseCtx *ctx, Token token, const char *fmt, ...) {
  if (ctx->panic_mode)
    return;

  va_list args;
  va_start(args, fmt);
  char *message = NULL;
  int msg_len = vasprintf(&message, fmt, args);
  va_end(args);

  if (msg_len < 0)
    return;

  if (ctx->diags) {
    unsigned int line = token.line;
    unsigned int col = token.col;
    unsigned int tok_len = token.len;

    diaglist_add(ctx->diags, DIAG_ERROR, message, ctx->lex->file, line, col,
                 line, col + tok_len);
  } else {
    fprintf(stderr, "Error at line %u, col %u: %s\n", token.line, token.col,
            message);
  }
  if (message)
    free(message);
  ctx->err_count++;
  ctx->panic_mode = true;
}

int get_precedence(Token op, bool is_unary, bool is_postfix) {
  if (op.len == 0)
    return 12;                           // Casting
  if (op.len == 1 && *op.start == '.') { // Member access
    return 14;
  }
  if (is_postfix)
    return 13;
  if (is_unary)
    return 12;

  if (op.type == TOKEN_ASSIGN)
    return 1;

  if (op.len == 1) {
    switch (*op.start) {
    case '*':
    case '/':
    case '%':
      return 11;
    case '+':
    case '-':
      return 10;
    case '<':
    case '>':
      return 8;
    case '&':
      return 6;
    case '^':
      return 5;
    case '|':
      return 4;
    }
  } else if (op.len == 2) {
    if (strncmp(op.start, "<<", 2) == 0 || strncmp(op.start, ">>", 2) == 0)
      return 9;
    if (strncmp(op.start, "<=", 2) == 0 || strncmp(op.start, ">=", 2) == 0)
      return 8;
    if (strncmp(op.start, "==", 2) == 0 || strncmp(op.start, "!=", 2) == 0)
      return 7;
    if (strncmp(op.start, "&&", 2) == 0)
      return 3;
    if (strncmp(op.start, "||", 2) == 0)
      return 2;

    bool assign_matches = false;
    switch (*op.start) {
    case '*':
    case '/':
    case '%':
    case '+':
    case '-':
    case '^':
      assign_matches = true;
    }
    if (assign_matches && op.start[1] == '=')
      return 1; // += etc
  } else if (op.len == 3) {
    if (strncmp(op.start, "<<=", 3) == 0 || strncmp(op.start, ">>=", 3) == 0)
      return 1;
  }
  return 0;
}

bool is_builtin_type_kw(ParseCtx *ctx, Token t) {
  if (t.type != TOKEN_KW)
    return false;
  return map_get(&ctx->lex->type_kw_map, t.start, t.len) != NULL;
}

void push_state(ParseCtx *ctx, ParseState state) {
  if (ctx->state_count >= ctx->state_cap) {
    size_t new_cap = (ctx->state_cap == 0) ? 32 : ctx->state_cap * 2;

    ParseState *new_stack =
        realloc(ctx->state_stack, sizeof(ParseState) * new_cap);

    if (!new_stack) {
      fprintf(stderr, "Out of memory: state_stack\n");
      exit(1);
    }

    ctx->state_stack = new_stack;
    ctx->state_cap = new_cap;
  }
  ctx->state_stack[ctx->state_count++] = state;
}

ParseState pop_state(ParseCtx *ctx) {
  if (ctx->state_count == 0) {
    fprintf(stderr, "Parser Error: State stack underflow\n");
    exit(1);
  }
  return ctx->state_stack[--ctx->state_count];
}

inline bool is_newline(char c) { return (c == '\n' || c == '\r'); }

void skip_irrelevant(LexCtx *ctx) {
  while (*ctx->curr != '\0') {
    if (isspace((unsigned char)*ctx->curr)) {
      if (is_newline(*ctx->curr)) {
        ctx->line++;
        ctx->col = 0;
      } else {
        ctx->col++;
      }
      ctx->curr++;
    } else if (strncmp(ctx->curr, "//", 2) == 0) {
      while (*ctx->curr != '\0' && !is_newline(*ctx->curr)) {
        ctx->curr++;
        ctx->col++;
      }
    } else {
      break;
    }
  }
}

Token next_token(ParseCtx *pctx) {
  LexCtx *ctx = pctx->lex;
  skip_irrelevant(ctx);

  ctx->start = ctx->curr;
  unsigned int tk_line = ctx->line;
  unsigned int tk_col = ctx->col;
  if (*ctx->curr == '\0') {
    return (Token){.start = ctx->start,
                   .len = 0,
                   .type = TOKEN_EOF,
                   .line = tk_line,
                   .col = tk_col};
  }

  TOKEN_TYPE type;
  unsigned int len = 0;

  // Identifiers, Keywords, Booleans
  if (isalpha((unsigned char)*ctx->curr) || *ctx->curr == '_') {
    while (isalnum((unsigned char)*ctx->curr) || *ctx->curr == '_') {
      ctx->curr++;
      ctx->col++;
    }
    len = ctx->curr - ctx->start;

    if ((len == 4 && strncmp(ctx->start, "true", 4) == 0) ||
        (len == 5 && strncmp(ctx->start, "false", 5) == 0)) {
      type = TOKEN_BOOL_LIT;
    } else if (is_kw(ctx, ctx->start, len)) {
      type = TOKEN_KW;
    } else {
      type = TOKEN_IDENTIF;
    }
  }

  // Numeric Literals
  else if (isdigit((unsigned char)*ctx->curr)) {
    bool has_dot = false;
    while (isdigit((unsigned char)*ctx->curr) || *ctx->curr == '.') {
      if (*ctx->curr == '.') {
        if (has_dot)
          break;
        has_dot = true;
      }
      ctx->curr++;
      ctx->col++;
    }
    len = ctx->curr - ctx->start;
    type = TOKEN_NUM_LIT;
  }

  // String/Char Literals
  else if (*ctx->curr == '"' || *ctx->curr == '\'') {
    char quote = *ctx->curr;
    ctx->curr++;
    ctx->col++;

    while (*ctx->curr != '\0' && *ctx->curr != quote) {
      if (*ctx->curr == '\\') {
        ctx->curr++;
        ctx->col++;
        char escape = *ctx->curr;
        switch (escape) {
        case 'n':
        case 't':
        case 'r':
        case '\\':
        case '"':
        case '\'':
          // Valid (not implementing unicode and all the other ones bro gimmie a
          // break)
          break;
        default:
          report_error(pctx, pctx->curr,
                       "Error: Invalid escape sequence \\%c\n", escape);
          return (Token){.start = ctx->start,
                         .len = (ctx->curr - ctx->start),
                         .type = TOKEN_UNKNOWN};
        }
        if (*ctx->curr == '\0')
          break;
        ctx->curr++;
        ctx->col++;
        continue;
      }

      if (is_newline(*ctx->curr)) {
        ctx->line++;
        ctx->col = 0;
      } else {
        ctx->col++;
      }
      ctx->curr++;
    }

    if (*ctx->curr == '\0') {
      report_error(pctx, pctx->curr,
                   "Error: Unterminated string at line %u, col %u col %u\n",
                   ctx->line, ctx->col, ctx->col);
      return (Token){.start = ctx->start,
                     .len = (ctx->curr - ctx->start),
                     .type = TOKEN_UNKNOWN};
    } else {
      if (quote == '\'' &&
          ((ctx->curr - ctx->start) > 3 ||
           (*ctx->curr == '\\' && (ctx->curr - ctx->start > 4)))) {
        report_error(pctx, pctx->curr, "Char literal must contain only 1 char");
        return (Token){.start = ctx->start,
                       .len = (ctx->curr - ctx->start),
                       .type = TOKEN_UNKNOWN};
      }
      ctx->curr++;
      ctx->col++;
      len = ctx->curr - ctx->start;
      type = (quote == '"') ? TOKEN_STR_LIT : TOKEN_CHAR_LIT;
    }
  }

  // Operators and Punctuation
  else {
    if (is_compare(ctx, ctx->curr, 3) || is_op(ctx, ctx->curr, 3)) {
      type = is_compare(ctx, ctx->curr, 3) ? TOKEN_COMPARE : TOKEN_OP;
      len = 3;
    } else if (is_compare(ctx, ctx->curr, 2) || is_op(ctx, ctx->curr, 2)) {
      type = is_compare(ctx, ctx->curr, 2) ? TOKEN_COMPARE : TOKEN_OP;
      len = 2;
    } else if (*ctx->curr == '=') {
      type = TOKEN_ASSIGN;
      len = 1;
    } else if (is_compare(ctx, ctx->curr, 1) || is_op(ctx, ctx->curr, 1) ||
               is_punc(*ctx->curr)) {
      len = 1;
      if (is_compare(ctx, ctx->curr, 1))
        type = TOKEN_COMPARE;
      else if (is_op(ctx, ctx->curr, 1))
        type = TOKEN_OP;
      else if (is_punc(*ctx->curr))
        type = TOKEN_PUNC;
      else
        type = TOKEN_UNKNOWN;
    } else {
      type = TOKEN_UNKNOWN;
      len = 1;
    }

    ctx->curr += len;
    ctx->col += len;
  }

  return (Token){.start = ctx->start,
                 .len = len,
                 .type = type,
                 .line = tk_line,
                 .col = tk_col};
}

Token peek_token(ParseCtx *pctx) {
  LexCtx *ctx = pctx->lex;
  char *saved_curr = ctx->curr;
  unsigned int saved_line = ctx->line;
  unsigned int saved_col = ctx->col;

  Token t = next_token(pctx);

  ctx->curr = saved_curr;
  ctx->line = saved_line;
  ctx->col = saved_col;

  return t;
}

void adv(ParseCtx *ctx) {
  ctx->prev = ctx->curr;
  ctx->curr = next_token(ctx);
}

void sync(ParseCtx *ctx) {
  while (ctx->curr.type != TOKEN_EOF) {
    if (ctx->prev.type == TOKEN_PUNC && *ctx->prev.start == ';') {
      ctx->panic_mode = false;
      adv(ctx);
      return;
    }

    if (ctx->curr.type == TOKEN_KW) {
      if (map_get(&ctx->lex->type_kw_map, ctx->curr.start, ctx->curr.len) ||
          (strncmp(ctx->curr.start, "struct", ctx->curr.len) == 0) ||
          (strncmp(ctx->curr.start, "enum", ctx->curr.len) == 0) ||
          (strncmp(ctx->curr.start, "extern", ctx->curr.len) == 0)) {
        ctx->panic_mode = false;
        return;
      }
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      ctx->panic_mode = false;
      return;
    }

    adv(ctx);
  }
}

void recover_state(ParseCtx *ctx, ParseState current_state) {
  push_state(ctx, current_state);

  // Pop states until we are at a safe area
  while (ctx->state_count > 0) {
    ParseState top = ctx->state_stack[ctx->state_count - 1];
    if (top == STATE_GLOBAL || top == STATE_PARSE_BLOCK ||
        top == STATE_IN_STRUCT_DEF || top == STATE_IN_UNION_DEF ||
        top == STATE_IN_ENUM_DEF || top == STATE_IN_EXTERN_BLOCK) {
      break;
    }
    pop_state(ctx);
  }

  ctx->op_count = 0;

  // Pop expressions until in safe area
  while (ctx->node_count > 0) {
    ASTN_TYPE t = ctx->node_stack[ctx->node_count - 1]->type;
    if (t == AST_PROGRAM || t == AST_BLOCK || t == AST_EXTERN ||
        t == AST_STRUCT || t == AST_UNION || t == AST_ENUM) {
      break;
    }
    ctx->node_count--;
  }
  ctx->panic_mode = false;
}

void push_node(ParseCtx *ctx, AstNode *node) {
  if (ctx->node_count >= ctx->node_cap) {
    size_t new_cap = (ctx->node_cap == 0) ? 32 : ctx->node_cap * 2;
    AstNode **new_stack = realloc(ctx->node_stack, sizeof(AstNode *) * new_cap);
    if (!new_stack) {
      fprintf(stderr,
              "No new stack returned after realloc needed for pushing node");
      exit(1);
    }
    ctx->node_stack = new_stack;
    ctx->node_cap = new_cap;
  }
  ctx->node_stack[ctx->node_count++] = node;
}

AstNode *pop_node(ParseCtx *ctx) {
  if (ctx->node_count == 0) {
    fprintf(stderr, "Parser Error: Node stack underflow\n");
    exit(1);
  }
  AstNode *node = ctx->node_stack[--ctx->node_count];
  if (node && !node->src_end) {
    node->src_end = ctx->prev.start + ctx->prev.len;
  }
  return node;
}

void push_op(ParseCtx *ctx, Token op, bool is_unary, bool is_postfix) {
  if (ctx->op_count >= ctx->op_cap) {
    size_t new_cap = (ctx->op_cap == 0) ? 32 : ctx->op_cap * 2;
    OpInfo *new_stack = realloc(ctx->op_stack, sizeof(OpInfo) * new_cap);
    if (!new_stack) {
      fprintf(stderr,
              "No new stack returned after realloc needed for pushing op");
      exit(1);
    }
    ctx->op_stack = new_stack;
    ctx->op_cap = new_cap;
  }
  ctx->op_stack[ctx->op_count++] = (OpInfo){op, is_unary, is_postfix, {0}};
}

void apply_op(ParseCtx *ctx) {
  if (ctx->op_count == 0)
    return;

  OpInfo info = ctx->op_stack[--ctx->op_count];

  if (info.is_unary) {
    if (ctx->node_count < 1)
      return;
    AstNode *operand = pop_node(ctx);

    if (info.op.len == 0) {
      AstNode *cast_node = new_node(ctx->arena, AST_CAST);
      cast_node->as.cast.target = info.cast_type;
      cast_node->as.cast.op = operand;
      push_node(ctx, cast_node);
      return;
    }

    ASTN_TYPE type = AST_UOP;

    // Specialization logic
    if (info.op.len == 1) {
      if (*info.op.start == '&')
        type = AST_ADDR_OF;
      else if (*info.op.start == '*')
        type = AST_DEREF;
    }

    AstNode *unop = new_node(ctx->arena, type);
    unop->as.unop.op = info.op;
    unop->as.unop.operand = operand;
    unop->as.unop.is_postfix = info.is_postfix;

    push_node(ctx, unop);
  } else {
    if (ctx->node_count < 2)
      return;
    AstNode *right = pop_node(ctx);
    AstNode *left = pop_node(ctx);

    if (info.op.len == 1 && *info.op.start == '.') {
      if (right->type != AST_IDENTIF && right->type != AST_FUNC_CALL) {
        report_error(ctx, ctx->curr, "Expected identifier after '.'");
        push_node(ctx, left);
        return;
      }

      Token member_name;
      if (right->type == AST_IDENTIF) {
        member_name = right->as.identif.val;
      } else {
        if (right->as.func_call.caller &&
            right->as.func_call.caller->type == AST_IDENTIF) {
          member_name = right->as.func_call.caller->as.identif.val;
        } else {
          report_error(ctx, ctx->curr, "Member call must be an identifier");
          push_node(ctx, left);
          return;
        }
      }

      AstNode *m_node = new_node(ctx->arena, AST_MEMBER);
      m_node->as.member.base = left;
      m_node->as.member.name = member_name;

      if (right->type == AST_FUNC_CALL) {
        right->as.func_call.caller = m_node;
        push_node(ctx, right);
      } else {
        push_node(ctx, m_node);
      }
      return;
    }
    AstNode *binop = new_node(ctx->arena, AST_BINOP);
    binop->as.binop.op = info.op;
    binop->as.binop.left = left;
    binop->as.binop.right = right;
    push_node(ctx, binop);
  }
}

bool is_type(ParseCtx *ctx) {
  ParseCtx tmp_parse = *ctx;
  LexCtx tmp_lex = *ctx->lex;
  tmp_parse.lex = &tmp_lex;
  Token t = ctx->curr;

  // Skip over any pointers or references
  while (t.type == TOKEN_OP && t.len == 1 &&
         (*t.start == '*' || *t.start == '&')) {
    t = next_token(&tmp_parse);
  }

  if (t.type == TOKEN_KW) {
    if (strncmp(t.start, "static", t.len) == 0 ||
        strncmp(t.start, "mut", t.len) == 0 ||
        strncmp(t.start, "threadlocal", t.len) == 0 ||
        strncmp(t.start, "extern", t.len) == 0) {
      return true;
    }
  }

  if (is_builtin_type_kw(ctx, t))
    return true;

  if (t.type == TOKEN_IDENTIF && t.len == 4 &&
      strncmp(t.start, "self", 4) == 0 && ctx->ag_depth > 0) {
    return true;
  }

  // Might be a custom type
  if (t.type == TOKEN_IDENTIF && !is_kw(ctx->lex, t.start, t.len)) {
    Token nxt = next_token(&tmp_parse);
    if (nxt.type == TOKEN_IDENTIF)
      return true;

    if (nxt.len == 1 && *nxt.start == '.') {
      Token nxt2 = next_token(&tmp_parse);
      if (nxt2.type == TOKEN_IDENTIF) {
        Token nxt3 = next_token(&tmp_parse);
        if (nxt3.type == TOKEN_IDENTIF)
          return true;
      }
    }
  }

  return false;
}

bool is_decl(ParseCtx *ctx) {
  if (ctx->curr.type == TOKEN_KW) {
    if (strncmp(ctx->curr.start, "inline", ctx->curr.len) == 0 ||
        strncmp(ctx->curr.start, "async", ctx->curr.len) == 0) {
      return true;
    }
  }
  return is_type(ctx);
}

inline bool is_lit_type(TOKEN_TYPE t) {
  return (t == TOKEN_NUM_LIT || t == TOKEN_STR_LIT || t == TOKEN_BOOL_LIT ||
          t == TOKEN_CHAR_LIT);
}

bool parse_step(ParseCtx *ctx) {
  ParseState current_state = pop_state(ctx);

  switch (current_state) {
  case STATE_IN_EXTERN_BLOCK:
  case STATE_GLOBAL: {
    bool is_extern = (current_state == STATE_IN_EXTERN_BLOCK);
    AstNode *container = ctx->node_stack[ctx->node_count - 1];
    AstNode **target_list = (container->type == AST_EXTERN)
                                ? &container->as.extern_block.contents
                                : &container->as.block.first_stmt;

    if (container->type == AST_EXTERN && ctx->curr.type == TOKEN_PUNC &&
        *ctx->curr.start == '}') {
      adv(ctx);
      pop_node(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_KW &&
        strncmp(ctx->curr.start, "extern", 6) == 0) {
      Token next = peek_token(ctx);
      if (next.type == TOKEN_KW && (strncmp(next.start, "struct", 6) == 0 ||
                                    strncmp(next.start, "union", 5) == 0)) {
        is_extern = true;
        adv(ctx);
      }
      if (next.type == TOKEN_PUNC && *next.start == '{') {
        adv(ctx);
        adv(ctx);

        AstNode *enode = new_node(ctx->arena, AST_EXTERN);
        append_stmt(target_list, enode);
        push_node(ctx, enode);

        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_EXTERN_BLOCK);
        break;
      }
    }

    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "struct", 6) == 0) {
        adv(ctx);
        AstNode *snode = new_node(ctx->arena, AST_STRUCT);
        snode->as.struct_def.structn = ctx->curr;
				snode->as.struct_def.is_extern = is_extern;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else if (is_extern && ctx->curr.type == TOKEN_PUNC &&
                   *ctx->curr.start == ';') {
          adv(ctx);
          snode->as.struct_def.contents = NULL;
          snode->src_end = ctx->prev.start + ctx->prev.len;
          append_stmt(target_list, snode);
          push_state(ctx, current_state);
          break;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after struct name");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        push_node(ctx, snode);
        append_stmt(target_list, snode);
        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_STRUCT_DEF);
        break;
      } else if (strncmp(ctx->curr.start, "union", 5) == 0) {
        adv(ctx);
        AstNode *unode = new_node(ctx->arena, AST_UNION);
        unode->as.union_def.unionn = ctx->curr;
				unode->as.struct_def.is_extern = is_extern;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else if (is_extern && ctx->curr.type == TOKEN_PUNC &&
                   *ctx->curr.start == ';') {
          adv(ctx);
          unode->as.union_def.contents = NULL;
          unode->src_end = ctx->prev.start + ctx->prev.len;
          append_stmt(target_list, unode);
          push_state(ctx, current_state);
          break;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after union name");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        push_node(ctx, unode);
        append_stmt(target_list, unode);
        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_UNION_DEF);
        break;
      } else if (strncmp(ctx->curr.start, "enum", 4) == 0) {
        adv(ctx);
        AstNode *enode = new_node(ctx->arena, AST_ENUM);
        enode->as.enum_def.enumn = ctx->curr;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after enum");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        push_node(ctx, enode);
        append_stmt(target_list, enode);
        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_ENUM_DEF);
        break;
      } else if (ctx->curr.type == TOKEN_KW &&
                 strncmp(ctx->curr.start, "use", 3) == 0) {
        Token use_kw = ctx->curr;
        adv(ctx);
        if (ctx->curr.type == TOKEN_STR_LIT) {
          Token path_token = ctx->curr;
          adv(ctx);

          Token alias_token = {0};
          Token semi_token = {0};

          if (ctx->curr.type == TOKEN_IDENTIF && ctx->curr.len == 2 &&
              strncmp(ctx->curr.start, "as", 2) == 0) {
            adv(ctx);
            if (ctx->curr.type == TOKEN_IDENTIF) {
              alias_token = ctx->curr;
              adv(ctx);
            } else {
              report_error(ctx, ctx->curr,
                           "Expected identifier after 'as' in use statement");
              adv(ctx);
              sync(ctx);
              recover_state(ctx, current_state);
              break;
            }
          }

          AstNode *use_node = new_node(ctx->arena, AST_USE);
          use_node->as.use_stmt.path = path_token;
          use_node->as.use_stmt.alias = alias_token;
          use_node->as.use_stmt.use_kw = use_kw;
          use_node->as.use_stmt.semicln = semi_token;
          append_stmt(target_list, use_node);

          if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
            semi_token = ctx->curr;
            adv(ctx);
          } else {
            report_error(ctx, ctx->curr, "Expected ';' after use");
            adv(ctx);
            sync(ctx);
            recover_state(ctx, current_state);
            break;
          }
          push_state(ctx, current_state);
          break;
        } else {
          report_error(ctx, ctx->curr,
                       "Expected string literal for module path");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
      }
    }

    if (is_type(ctx) || is_decl(ctx)) {
      DataType type = parse_type(ctx);

      if (ctx->curr.type != TOKEN_IDENTIF &&
          !is_builtin_type_kw(ctx, ctx->curr)) {
        report_error(ctx, ctx->curr, "Expected identifier after type");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
      Token name = ctx->curr;
      adv(ctx);

      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
        if (type.is_threadlocal) {
          report_error(
              ctx, ctx->curr,
              "Error: 'threadlocal' cannot be applied to function '%.*s' "
              "at line %u, col %u\n",
              name.len, name.start, ctx->lex->line, ctx->lex->col);
          return false;
        }
        AstNode *fnode = new_node(ctx->arena, AST_FUNC);
        fnode->as.func_def.fn_name = name;
        fnode->as.func_def.ret_type = type;
        fnode->as.func_def.is_async = type.is_async;
        fnode->as.func_def.is_inline = type.is_inline;
        if (current_state == STATE_IN_EXTERN_BLOCK || type.is_extern) {
          fnode->as.func_def.is_extern = true;
        }
        adv(ctx);

        AstNode *params_head = NULL;
        AstNode *params_tail = NULL;
        while (ctx->curr.type != TOKEN_EOF &&
               !(ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')')) {
          if (!is_type(ctx)) {
            report_error(ctx, ctx->curr,
                         "Expected type in function parameters");
            adv(ctx);
            sync(ctx);
            recover_state(ctx, current_state);
            break;
          }
          DataType p_type = parse_type(ctx);
          Token p_name;

          if (p_type.is_self) {
            report_error(
                ctx, ctx->curr,
                "Using self is not allowed when not within a struct union "
                "or enum on line %u, col %u",
                ctx->lex->line, ctx->lex->col);
            return false;
          } else {
            if (ctx->curr.type != TOKEN_IDENTIF &&
                !is_builtin_type_kw(ctx, ctx->curr)) {
              report_error(
                  ctx, ctx->curr,
                  "Expected identifier after type in params at line %u, "
                  "col %u\n",
                  ctx->lex->line, ctx->lex->col);
              return false;
            }
            p_name = ctx->curr;
            adv(ctx);
          }

          if (p_type.is_async || p_type.is_inline) {
            report_error(ctx, ctx->curr,
                         "Error: Invalid modifier on parameter");
            adv(ctx);
            sync(ctx);
            recover_state(ctx, current_state);
            break;
          }

          AstNode *pnode = new_node(ctx->arena, AST_PARAM);
          pnode->as.fn_param.type = p_type;
          pnode->as.fn_param.id = p_name;

          if (!params_head)
            params_head = params_tail = pnode;
          else {
            params_tail->next = pnode;
            params_tail = pnode;
          }

          if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',')
            adv(ctx);
        }
        fnode->as.func_def.params = params_head;
        adv(ctx);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
          if (!fnode->as.func_def.is_extern) {
            report_error(
                ctx, ctx->curr,
                "Error: Function prototype '%.*s' must be marked 'extern' "
                "at line %u, col %u\n",
                name.len, name.start, ctx->lex->line, ctx->lex->col);
            return false;
          }
          adv(ctx);
          fnode->as.func_def.block = NULL;
          fnode->src_end = ctx->prev.start + ctx->prev.len;
          append_stmt(target_list, fnode);
          push_state(ctx, current_state);
          break;
        }

        push_node(ctx, fnode);
        append_stmt(target_list, fnode);

        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_FUNC);
        break;
      } else {
        if (type.is_async || type.is_inline) {
          fprintf(
              stderr,
              "Error: Invalid modifier on variable '%.*s' at line %u, col %u\n",
              name.len, name.start, ctx->lex->line, ctx->lex->col);
          return false;
        }
        AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
        vnode->as.var_decl.type = type;
        vnode->as.var_decl.id = name;

        append_stmt(target_list, vnode);

        if (ctx->curr.type == TOKEN_ASSIGN) {
          adv(ctx);
          push_node(ctx, vnode);
          push_state(ctx, current_state);
          push_state(ctx, STATE_VAR_INIT_DONE);
          ctx->expect_operand = true;
          push_state(ctx, STATE_IN_EXPR);
          break;
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
          adv(ctx);
          vnode->src_end = ctx->prev.start + ctx->prev.len;
        }
        push_state(ctx, current_state);
        break;
      }
    }
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
      push_state(ctx, current_state);
      break;
    }
    report_error(ctx, ctx->curr, "Unexpected token in global scope");
    adv(ctx);
    sync(ctx);
    recover_state(ctx, current_state);
    break;
  }
  case STATE_EXPR_STMT_DONE: {
    AstNode *expr_node = pop_node(ctx);
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

    if (parent->type == AST_VAR_DECL) {
      parent->as.var_decl.init = expr_node;
    } else if (parent->type == AST_BLOCK || parent->type == AST_PROGRAM) {
      append_stmt(&parent->as.block.first_stmt, expr_node);
    } else {
      report_error(
          ctx, ctx->curr,
          "Parser Error: Unexpected context for expression at line %u, col "
          "%u\n",
          ctx->lex->line, ctx->lex->col);
      return false;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    }
    break;
  }
  case STATE_IN_EXPR: {
    if (ctx->curr.type == TOKEN_EOF ||
        (ctx->curr.type == TOKEN_PUNC &&
         (*ctx->curr.start == ';' || *ctx->curr.start == ')' ||
          *ctx->curr.start == ',' || *ctx->curr.start == '}' ||
          *ctx->curr.start == ']'))) {

      ctx->expect_operand = true;

      while (ctx->op_count > 0) {
        OpInfo *top = &ctx->op_stack[ctx->op_count - 1];

        // If null its a cast not (
        if (top->op.start != NULL && *top->op.start == '(') {
          break;
        }

        apply_op(ctx);
      }
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
      if (!ctx->expect_operand) {
        Token open_paren = ctx->curr;

        AstNode *caller = pop_node(ctx);
        AstNode *call_node = new_node(ctx->arena, AST_FUNC_CALL);
        call_node->as.func_call.caller = caller;
        push_node(ctx, call_node);
        adv(ctx);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
          adv(ctx);
          push_state(ctx, STATE_IN_EXPR);
          break;
        }
        push_op(ctx, open_paren, false, false);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_FUNC_ARGS);
        break;
      } else {
        // This might be grouping or cast
        LexCtx saved_lex = *ctx->lex;
        Token saved_curr = ctx->curr;
        Token saved_prev = ctx->prev;

        adv(ctx);
        bool is_cast = is_type(ctx); // Check if the next tokens form a valid
                                     // type for (type)identifier syntax

        // Go back to the ( that was skipped
        *ctx->lex = saved_lex;
        ctx->curr = saved_curr;
        ctx->prev = saved_prev;

        if (is_cast) {
          adv(ctx);
          DataType cast_t = parse_type(ctx);

          if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
            adv(ctx);

            // Push a fake unary operator (len = 0 is a cast)
            Token fake_op = {.start = NULL, .len = 0, .type = TOKEN_OP};
            push_op(ctx, fake_op, true, false);

            // Add type info so apply_op can work with it
            ctx->op_stack[ctx->op_count - 1].cast_type = cast_t;

            ctx->expect_operand = true; // Expect expression to be casted
            push_state(ctx, STATE_IN_EXPR);
            break;
          } else {
            report_error(ctx, ctx->curr, "Expected ')' after cast type");
            adv(ctx);
            sync(ctx);
            recover_state(ctx, current_state);
            break;
          }
        }

        // Grouping brackets
        push_op(ctx, ctx->curr, false, false);
        ctx->expect_operand = true;
        adv(ctx);
        push_state(ctx, STATE_IN_EXPR);
        break;
      }
    }

    bool is_async_block = false;
    if (ctx->curr.type == TOKEN_KW && ctx->curr.len == 5 &&
        strncmp(ctx->curr.start, "async", 5) == 0) {
      Token next = peek_token(ctx);
      if (next.type == TOKEN_PUNC && *next.start == '{') {
        is_async_block = true;
        adv(ctx);
      }
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *block_node = new_node(ctx->arena, AST_BLOCK);
      block_node->as.block.is_async = is_async_block;
      push_node(ctx, block_node);

      push_state(ctx, STATE_IN_EXPR);
      push_state(ctx, STATE_BLOCK_EXPR_DONE);
      push_state(ctx, STATE_PARSE_BLOCK);

      // Ensure block internals dont consume outer operators
      Token dummy = {.start = "(", .len = 1, .type = TOKEN_PUNC};
      push_op(ctx, dummy, false, false);

      ctx->expect_operand = false;
      break;
    }

    if (ctx->curr.type == TOKEN_KW && ctx->curr.len == 6 &&
        strncmp(ctx->curr.start, "sizeof", 6) == 0) {

      adv(ctx);

      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
        adv(ctx);

        AstNode *sz_node = new_node(ctx->arena, AST_SIZEOF);

        if (is_type(ctx)) {
          sz_node->as.sizeof_expr.is_type = true;
          sz_node->as.sizeof_expr.target_type = parse_type(ctx);

          if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
            adv(ctx);
            push_node(ctx, sz_node);
            ctx->expect_operand = false;
            push_state(ctx, STATE_IN_EXPR);
            break;
          } else {
            report_error(ctx, ctx->curr, "Expected ')' after sizeof type");
            adv(ctx);
            sync(ctx);
            recover_state(ctx, current_state);
            break;
          }
        } else {
          sz_node->as.sizeof_expr.is_type = false;
          push_node(ctx, sz_node);

          push_state(ctx, STATE_IN_EXPR);
          push_state(ctx, STATE_SIZEOF_EXPR_DONE);
          ctx->expect_operand = true;
          push_state(ctx, STATE_IN_EXPR);
          break;
        }
      } else {
        report_error(ctx, ctx->curr, "Expected '(' after sizeof");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
    }

    if (ctx->curr.type == TOKEN_IDENTIF || is_lit_type(ctx->curr.type) ||
        (ctx->curr.type == TOKEN_KW &&
         strncmp(ctx->curr.start, "null", 4) == 0) ||
        is_builtin_type_kw(ctx, ctx->curr)) {
      ctx->expect_operand = false;

      ASTN_TYPE node_type;

      switch (ctx->curr.type) {
      case TOKEN_IDENTIF:
        node_type = AST_IDENTIF;
        break;
      case TOKEN_NUM_LIT:
        node_type = AST_NUM_LIT;
        break;
      case TOKEN_STR_LIT:
        node_type = AST_STR_LIT;
        break;
      case TOKEN_CHAR_LIT:
        node_type = AST_CHAR_LIT;
        break;
      case TOKEN_BOOL_LIT:
        node_type = AST_BOOL_LIT;
        break;
      case TOKEN_KW:
        if (strncmp(ctx->curr.start, "null", 4) == 0)
          node_type = AST_NULL_LIT;
        else
          node_type = AST_IDENTIF;
        break;
      default:
        return false;
      }

      AstNode *node = new_node(ctx->arena, node_type);

      if (node_type == AST_STR_LIT)
        node->as.str_lit.val = ctx->curr;
      else if (node_type == AST_CHAR_LIT)
        node->as.char_lit.val = ctx->curr;
      else if (node_type == AST_NUM_LIT)
        node->as.num_lit.val = ctx->curr;
      else if (node_type == AST_NULL_LIT)
        node->as.null_lit.val = ctx->curr;
      else
        node->as.identif.val = ctx->curr;

      push_node(ctx, node);
      adv(ctx);
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_OP || ctx->curr.type == TOKEN_COMPARE ||
        ctx->curr.type == TOKEN_ASSIGN) {
      bool is_unary = false;
      bool is_postfix = false;

      if (ctx->expect_operand) {
        is_unary = true;
      } else {
        if (ctx->curr.len == 2 && (strncmp(ctx->curr.start, "++", 2) == 0 ||
                                   strncmp(ctx->curr.start, "--", 2) == 0)) {
          is_unary = true;
          is_postfix = true;
        } else {
          ctx->expect_operand = true;
        }
      }

      if (!is_unary && ctx->curr.len == 1 && *ctx->curr.start == '.') {
        Token next = peek_token(ctx);
        if (next.type != TOKEN_IDENTIF && next.type != TOKEN_KW) {
          report_error(ctx, ctx->curr, "Expected identifier after '.'");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
      }

      int current_prec = get_precedence(ctx->curr, is_unary, is_postfix);

      bool left_assoc =
          !(is_unary && !is_postfix) && (ctx->curr.type != TOKEN_ASSIGN);

      while (ctx->op_count > 0 &&
             *ctx->op_stack[ctx->op_count - 1].op.start != '(') {
        OpInfo top_op = ctx->op_stack[ctx->op_count - 1];
        int top_prec =
            get_precedence(top_op.op, top_op.is_unary, top_op.is_postfix);

        if ((left_assoc && top_prec >= current_prec) ||
            (!left_assoc && top_prec > current_prec)) {
          apply_op(ctx);
        } else {
          break;
        }
      }

      push_op(ctx, ctx->curr, is_unary, is_postfix);
      adv(ctx);
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '[') {
      if (ctx->expect_operand) {
        // Array lit
        adv(ctx);
        AstNode *array_node = new_node(ctx->arena, AST_ARRAY_LIT);
        push_node(ctx, array_node);
        push_state(ctx, STATE_IN_ARRAY_LIT);
        break;
      } else {
        // Indexing
        adv(ctx);
        AstNode *base = pop_node(ctx);
        AstNode *idx_node = new_node(ctx->arena, AST_INDEX);
        idx_node->as.index.base = base;
        push_node(ctx, idx_node);

        // Preventing from eating outer opts
        Token dummy = {.start = "(", .len = 1, .type = TOKEN_PUNC};
        push_op(ctx, dummy, false, false);

        push_state(ctx, STATE_IN_EXPR);
        push_state(ctx, STATE_INDEX_DONE);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_EXPR);
        break;
      }
    }

    report_error(ctx, ctx->curr, "Unexpected token in expression");
    adv(ctx);
    sync(ctx);
    recover_state(ctx, current_state);
    break;
  }

  case STATE_SIZEOF_EXPR_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *sz_node = ctx->node_stack[ctx->node_count - 1];
    sz_node->as.sizeof_expr.target_expr = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
      ctx->expect_operand = false;
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after sizeof expression");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_INDEX_DONE: {
    AstNode *index_expr = pop_node(ctx);
    AstNode *idx_node = ctx->node_stack[ctx->node_count - 1];
    idx_node->as.index.index = index_expr;

    // Remove the dummy
    if (ctx->op_count > 0 &&
        *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
      ctx->op_count--;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      ctx->expect_operand = false;
    } else {
      report_error(ctx, ctx->curr, "Expected ']' after index");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_IN_ARRAY_LIT: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      ctx->expect_operand = false;
      break;
    }

    if (ctx->curr.type == TOKEN_EOF ||
        (ctx->curr.type == TOKEN_PUNC &&
         (*ctx->curr.start == ';' || *ctx->curr.start == '}'))) {
      report_error(ctx, ctx->curr, "Unexpected token in function arguments");
      recover_state(ctx, current_state);
      break;
    }

    push_state(ctx, STATE_IN_ARRAY_LIT);
    push_state(ctx, STATE_ARRAY_ELEMENT_DONE);
    ctx->expect_operand = true;
    push_state(ctx, STATE_IN_EXPR);
    break;
  }

  case STATE_ARRAY_ELEMENT_DONE: {
    AstNode *element_expr = pop_node(ctx);
    AstNode *array_node = ctx->node_stack[ctx->node_count - 1];

    // Append the element to the array's linked list
    if (array_node->as.array_lit.elements == NULL) {
      array_node->as.array_lit.elements = element_expr;
    } else {
      AstNode *curr = array_node->as.array_lit.elements;
      while (curr->next)
        curr = curr->next;
      curr->next = element_expr;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      pop_state(ctx);
      ctx->expect_operand = false;
    } else {
      report_error(ctx, ctx->curr, "Expected ',' or ']' in array literal");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }
  case STATE_BLOCK_EXPR_DONE: {
    // Remove dummy
    if (ctx->op_count > 0 &&
        *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
      ctx->op_count--;
    }
    break;
  }
  case STATE_IN_FUNC: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      AstNode *func_node = ctx->node_stack[ctx->node_count - 1];
      AstNode *block_node = new_node(ctx->arena, AST_BLOCK);
      func_node->as.func_def.block = block_node;

      push_node(ctx, block_node);
      push_state(ctx, STATE_FUNC_BODY_DONE);
      push_state(ctx, STATE_BLOCK_DONE);
      push_state(ctx, STATE_PARSE_BLOCK);
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected '{' to start function body");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_FUNC_BODY_DONE: {
    pop_node(ctx);
    break;
  }

  case STATE_BLOCK_DONE: {
    pop_node(ctx);
    break;
  }

  case STATE_PARSE_BLOCK: {
    AstNode *current_block = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_IDENTIF && ctx->curr.len == 4 &&
        strncmp(ctx->curr.start, "self", 4) == 0) {
      push_state(ctx, STATE_PARSE_BLOCK);
      push_state(ctx, STATE_EXPR_STMT_DONE);
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);

      AstNode *nested_block = new_node(ctx->arena, AST_BLOCK);
      append_stmt(&current_block->as.block.first_stmt, nested_block);

      push_state(ctx, STATE_PARSE_BLOCK);
      push_state(ctx, STATE_BLOCK_DONE);
      push_node(ctx, nested_block);
      push_state(ctx, STATE_PARSE_BLOCK);
      break;
    }

    push_state(ctx, STATE_PARSE_BLOCK);

    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "struct", 6) == 0 ||
          strncmp(ctx->curr.start, "union", 5) == 0 ||
          strncmp(ctx->curr.start, "enum", 4) == 0) {
        Token type_kw = ctx->curr;
        adv(ctx);
        AstNode *local_type = NULL;
        if (strncmp(type_kw.start, "struct", 6) == 0) {
          local_type = new_node(ctx->arena, AST_STRUCT);
          local_type->as.struct_def.structn = ctx->curr;
          adv(ctx);
        } else if (strncmp(type_kw.start, "union", 5) == 0) {
          local_type = new_node(ctx->arena, AST_UNION);
          local_type->as.union_def.unionn = ctx->curr;
          adv(ctx);
        } else {
          local_type = new_node(ctx->arena, AST_ENUM);
          local_type->as.enum_def.enumn = ctx->curr;
          adv(ctx);
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after local type name");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        append_stmt(&current_block->as.block.first_stmt, local_type);
        push_node(ctx, local_type);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, (local_type->type == AST_STRUCT)  ? STATE_IN_STRUCT_DEF
                        : (local_type->type == AST_UNION) ? STATE_IN_UNION_DEF
                                                          : STATE_IN_ENUM_DEF);
        break;
      } else if (strncmp(ctx->curr.start, "if", 2) == 0) {
        adv(ctx);
        AstNode *if_node = new_node(ctx->arena, AST_IF);
        append_stmt(&current_block->as.block.first_stmt, if_node);
        push_node(ctx, if_node);

        push_state(ctx, STATE_IF_BODY_DONE);
        push_state(ctx, STATE_IF_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after if\n");
          return false;
        }
        break;
      } else if (strncmp(ctx->curr.start, "while", 5) == 0) {
        adv(ctx);
        AstNode *while_node = new_node(ctx->arena, AST_WHILE);
        append_stmt(&current_block->as.block.first_stmt, while_node);
        push_node(ctx, while_node);

        push_state(ctx, STATE_WHILE_BODY_DONE);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, STATE_WHILE_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after while\n");
          return false;
        }
        break;
      } else if (strncmp(ctx->curr.start, "defer", 5) == 0) {
        adv(ctx);
        AstNode *defer_node = new_node(ctx->arena, AST_DEFER);
        append_stmt(&current_block->as.block.first_stmt, defer_node);

        push_node(ctx, defer_node);
        push_state(ctx, STATE_DEFER_DONE);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
          push_node(ctx, body_block);
          push_state(ctx, STATE_PARSE_BLOCK);
          adv(ctx);
        } else {
          push_state(ctx, STATE_IN_EXPR);
        }
        break;
      } else if (strncmp(ctx->curr.start, "break", 5) == 0) {
        Token kw = ctx->curr;
        adv(ctx);
        AstNode *brk = new_node(ctx->arena, AST_BREAK);
        brk->as.break_stmt.kw = kw;
        append_stmt(&current_block->as.block.first_stmt, brk);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';')
          adv(ctx);
        break;
      } else if (strncmp(ctx->curr.start, "continue", 8) == 0) {
        Token kw = ctx->curr;
        adv(ctx);
        AstNode *cnt = new_node(ctx->arena, AST_CONTINUE);
        cnt->as.continue_stmt.kw = kw;
        append_stmt(&current_block->as.block.first_stmt, cnt);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';')
          adv(ctx);
        break;
      } else if (strncmp(ctx->curr.start, "ret", 3) == 0) {
        Token ret_token = ctx->curr;
        adv(ctx);

        AstNode *ret_node = new_node(ctx->arena, AST_RET);
        ret_node->as.ret_stmt.ret_kw = ret_token;

        append_stmt(&current_block->as.block.first_stmt, ret_node);

        push_node(ctx, ret_node);

        push_state(ctx, STATE_RET_DONE);
        push_state(ctx, STATE_IN_EXPR);
        break;
      } else if (strncmp(ctx->curr.start, "for", 3) == 0) {
        adv(ctx);
        AstNode *for_node = new_node(ctx->arena, AST_FOR);
        append_stmt(&current_block->as.block.first_stmt, for_node);
        push_node(ctx, for_node);

        push_state(ctx, STATE_FOR_BODY_DONE);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, STATE_FOR_INC_DONE);
        push_state(ctx, STATE_IN_EXPR);
        push_state(ctx, STATE_FOR_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        push_state(ctx, STATE_FOR_INIT_START);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after for\n");
          return false;
        }
        break;
      } else if (strncmp(ctx->curr.start, "switch", 6) == 0) {
        adv(ctx);
        AstNode *switch_node = new_node(ctx->arena, AST_SWITCH);
        append_stmt(&current_block->as.block.first_stmt, switch_node);
        push_node(ctx, switch_node);

        push_state(ctx, STATE_SWITCH_DONE);
        push_state(ctx, STATE_PARSE_SWITCH_BODY);
        push_state(ctx, STATE_SWITCH_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after switch");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        break;
      }
    }
    if (is_type(ctx)) {
      DataType type = parse_type(ctx);
      if (type.is_async) {
        report_error(
            ctx, ctx->curr,
            "Error: 'async' cannot be applied to variables at line %u, col "
            "%u\n",
            ctx->lex->line, ctx->lex->col);
        return false;
      }
      if (type.is_inline) {
        report_error(
            ctx, ctx->curr,
            "Error: 'inline' cannot be applied to variables at line %u, "
            "col %u\n",
            ctx->lex->line, ctx->lex->col);
        return false;
      }
      if (ctx->curr.type != TOKEN_IDENTIF) {
        report_error(ctx, ctx->curr, "Expected identifier after type");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
      Token name = ctx->curr;
      adv(ctx);

      AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
      vnode->as.var_decl.type = type;
      vnode->as.var_decl.id = name;
      append_stmt(&current_block->as.block.first_stmt, vnode);

      if (ctx->curr.type == TOKEN_ASSIGN) {
        adv(ctx);
        push_node(ctx, vnode);
        push_state(ctx, STATE_VAR_INIT_DONE);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_EXPR);
      } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        adv(ctx);
      } else {
        report_error(ctx, ctx->curr, "Expected ';' or '=' after variable name");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
      break;
    }

    bool is_expr_start = false;
    if (ctx->curr.type == TOKEN_IDENTIF || is_lit_type(ctx->curr.type) ||
        ctx->curr.type == TOKEN_OP) {
      is_expr_start = true;
    } else if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "sizeof", 6) == 0 ||
          strncmp(ctx->curr.start, "null", 4) == 0 ||
          is_builtin_type_kw(ctx, ctx->curr)) {
        is_expr_start = true;
      }
    } else if (ctx->curr.type == TOKEN_PUNC) {
      if (*ctx->curr.start == '(' || *ctx->curr.start == '[') {
        is_expr_start = true;
      }
    }

    if (is_expr_start) {
      push_state(ctx, STATE_EXPR_STMT_DONE);
      push_state(ctx, STATE_IN_EXPR);
    } else {
      report_error(ctx, ctx->curr, "Unexpected token in block");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
    break;
  }

  case STATE_FOR_INIT_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.init = init_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr,
                   "Expected ';' after for-loop initialization");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_FOR_INIT_START: {
    bool is_decl = false;

    if (is_builtin_type_kw(ctx, ctx->curr) ||
        (ctx->curr.type == TOKEN_KW &&
         (strncmp(ctx->curr.start, "mut", 3) == 0 ||
          strncmp(ctx->curr.start, "static", 6) == 0))) {
      is_decl = true;
    } else if (ctx->curr.type == TOKEN_IDENTIF) {
      Token next = peek_token(ctx);
      if (next.type == TOKEN_IDENTIF) {
        is_decl = true;
      }
    }

    if (is_decl) {
      DataType type = parse_type(ctx);
      if (type.is_async) {
        report_error(
            ctx, ctx->curr,
            "Error: 'async' cannot be applied to variables at line %u, col "
            "%u\n",
            ctx->lex->line, ctx->lex->col);
        return false;
      }
      if (type.is_inline) {
        report_error(
            ctx, ctx->curr,
            "Error: 'inline' cannot be applied to variables at line %u, "
            "col %u\n",
            ctx->lex->line, ctx->lex->col);
        return false;
      }
      if (ctx->curr.type != TOKEN_IDENTIF) {
        report_error(ctx, ctx->curr, "Expected identifier after type");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
      Token name = ctx->curr;
      adv(ctx);

      AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
      vnode->as.var_decl.type = type;
      vnode->as.var_decl.id = name;

      push_node(ctx, vnode);

      if (ctx->curr.type == TOKEN_ASSIGN) {
        adv(ctx);
        push_state(ctx, STATE_FOR_INIT_DECL_DONE);
        push_state(ctx, STATE_IN_EXPR);
      } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        AstNode *var_node = pop_node(ctx);
        AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
        for_node->as.for_loop.init = var_node;
        adv(ctx);
      } else {
        report_error(
            ctx, ctx->curr,
            "Expected '=' or ';' after variable declaration in for loop "
            "at line %u, col %u\n",
            ctx->lex->line, ctx->lex->col);
        return false;
      }
    } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
      for_node->as.for_loop.init = NULL;
      adv(ctx);
    } else {
      push_state(ctx, STATE_FOR_INIT_DONE);
      push_state(ctx, STATE_IN_EXPR);
    }
    break;
  }

  case STATE_FOR_INIT_DECL_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *var_node = pop_node(ctx);
    var_node->as.var_decl.init = init_expr;

    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.init = var_node;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      report_error(
          ctx, ctx->curr,
          "Expected ';' after for-loop variable declaration at line %u, "
          "col %u\n",
          ctx->lex->line, ctx->lex->col);
      return false;
    }
    break;
  }

  case STATE_FOR_COND_DONE: {
    AstNode *cond_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.check = cond_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ';' after for-loop condition");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_FOR_INC_DONE: {
    AstNode *inc_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.inc = inc_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after for-loop increment");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected '{' to start for-loop body");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_FOR_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *for_node = pop_node(ctx);
    for_node->as.for_loop.action = body_block;
    break;
  }

  case STATE_WHILE_COND_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *while_node = ctx->node_stack[ctx->node_count - 1];
    while_node->as.while_loop.check = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after while-condition\n");
      return false;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
    } else {
      report_error(ctx, ctx->curr, "Expected '{' for while body\n");
      return false;
    }
    break;
  }

  case STATE_WHILE_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *while_node = pop_node(ctx);
    while_node->as.while_loop.action = body_block;
    break;
  }

  case STATE_DEFER_DONE: {
    AstNode *action = pop_node(ctx);
    AstNode *defer_node = pop_node(ctx);
    defer_node->as.defer_stmt.contents = action;
    break;
  }
  case STATE_IF_COND_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.check = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after if-condition");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
      push_state(ctx, STATE_PARSE_BLOCK);
    } else {
      push_state(ctx, STATE_IN_EXPR);
    }
    break;
  }
  case STATE_IF_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.action = body_block;

    if (ctx->curr.type == TOKEN_KW &&
        strncmp(ctx->curr.start, "else", 4) == 0) {
      adv(ctx);

      push_state(ctx, STATE_IF_ELSE_DONE);

      if (ctx->curr.type == TOKEN_KW &&
          strncmp(ctx->curr.start, "if", 2) == 0) {
        // Else if block
        adv(ctx);
        AstNode *elif_node = new_node(ctx->arena, AST_IF);
        if_node->as.if_check.elseAct = elif_node;

        push_node(ctx, elif_node);
        push_state(ctx, STATE_IF_BODY_DONE);
        push_state(ctx, STATE_IF_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after else if\n");
          return false;
        }
      } else {
        // Else block
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          AstNode *else_block = new_node(ctx->arena, AST_BLOCK);
          if_node->as.if_check.elseAct = else_block;

          push_state(ctx, STATE_ELSE_BODY_DONE);
          push_node(ctx, else_block);
          push_state(ctx, STATE_PARSE_BLOCK);
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after else\n");
          return false;
        }
      }
    } else {
      // No else
      pop_node(ctx);
    }
    break;
  }

  case STATE_ELSE_BODY_DONE: {
    pop_node(ctx);
    break;
  }

  case STATE_IF_ELSE_DONE: {
    pop_node(ctx);
    break;
  }
  case STATE_IN_IF_EXPECT_COND: {
    AstNode *cond_expr = pop_node(ctx);

    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.check = cond_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after if-condition");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }
  case STATE_IN_STRUCT_DEF:
  case STATE_IN_UNION_DEF: {
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      ctx->ag_depth--;
      pop_node(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "struct", 6) == 0 ||
          strncmp(ctx->curr.start, "union", 5) == 0 ||
          strncmp(ctx->curr.start, "enum", 4) == 0) {
        Token type_kw = ctx->curr;
        adv(ctx);
        AstNode *nested_type = NULL;
        if (strncmp(type_kw.start, "struct", 6) == 0) {
          nested_type = new_node(ctx->arena, AST_STRUCT);
          nested_type->as.struct_def.structn = ctx->curr;
          adv(ctx);
        } else if (strncmp(type_kw.start, "union", 5) == 0) {
          nested_type = new_node(ctx->arena, AST_UNION);
          nested_type->as.union_def.unionn = ctx->curr;
          adv(ctx);
        } else { // enum
          nested_type = new_node(ctx->arena, AST_ENUM);
          nested_type->as.enum_def.enumn = ctx->curr;
          adv(ctx);
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after nested type name");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        AstNode **target_list = (parent->type == AST_STRUCT)
                                    ? &parent->as.struct_def.contents
                                    : &parent->as.union_def.contents;
        append_stmt(target_list, nested_type);
        push_node(ctx, nested_type);
        push_state(ctx, current_state);
        push_state(ctx, (nested_type->type == AST_STRUCT)  ? STATE_IN_STRUCT_DEF
                        : (nested_type->type == AST_UNION) ? STATE_IN_UNION_DEF
                                                           : STATE_IN_ENUM_DEF);
        break;
      }
    }

    if (!is_type(ctx)) {
      report_error(ctx, ctx->curr, "Expected type in struct/union definition");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    DataType field_type = parse_type(ctx);

    if (field_type.is_async) {
      report_error(
          ctx, ctx->curr,
          "Error: 'async' cannot be applied to struct/union fields at line "
          "%u\n",
          ctx->lex->line);
      return false;
    }
    if (field_type.is_inline) {
      report_error(
          ctx, ctx->curr,
          "Error: 'inline' cannot be applied to struct/union fields at "
          "line %u, col %u\n",
          ctx->lex->line, ctx->lex->col);
      return false;
    }

    if (ctx->curr.type != TOKEN_IDENTIF &&
        !is_builtin_type_kw(ctx, ctx->curr)) {
      report_error(ctx, ctx->curr, "Expected field/method identifier");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    Token name = ctx->curr;
    adv(ctx);

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
      if (field_type.is_threadlocal) {
        report_error(
            ctx, ctx->curr,
            "Error: 'threadlocal' cannot be applied to method '%.*s' at "
            "line %u, col %u\n",
            name.len, name.start, ctx->lex->line, ctx->lex->col);
        return false;
      }
      AstNode *fnode = new_node(ctx->arena, AST_FUNC);
      fnode->as.func_def.fn_name = name;
      fnode->as.func_def.ret_type = field_type;
      fnode->as.func_def.is_async = field_type.is_async;
      fnode->as.func_def.is_inline = field_type.is_inline;
      fnode->as.func_def.is_extern = field_type.is_extern;
      adv(ctx);

      // Parse parameters
      AstNode *params_head = NULL;
      AstNode *params_tail = NULL;
      while (ctx->curr.type != TOKEN_EOF &&
             !(ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')')) {
        if (!is_type(ctx)) {
          report_error(ctx, ctx->curr, "Expected type in function parameters");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        DataType p_type = parse_type(ctx);
        Token p_name;

        if (p_type.is_self) {
          if (ctx->curr.type == TOKEN_IDENTIF) {
            p_name = ctx->curr;
            adv(ctx);
          } else {
            p_name.start = "self";
            p_name.len = 4;
            p_name.type = TOKEN_IDENTIF;
          }
        } else {
          if (ctx->curr.type != TOKEN_IDENTIF &&
              !is_builtin_type_kw(ctx, ctx->curr)) {
            fprintf(
                stderr,
                "Expected identifier after type in params at line %u, col %u\n",
                ctx->lex->line, ctx->lex->col);
            return false;
          }
          p_name = ctx->curr;
          adv(ctx);
        }

        if (p_type.is_async || p_type.is_inline) {
          report_error(ctx, ctx->curr, "Error: Invalid modifier on parameter");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }

        AstNode *pnode = new_node(ctx->arena, AST_PARAM);
        pnode->as.fn_param.type = p_type;
        pnode->as.fn_param.id = p_name;
        if (!params_head)
          params_head = params_tail = pnode;
        else {
          params_tail->next = pnode;
          params_tail = pnode;
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',')
          adv(ctx);
      }
      fnode->as.func_def.params = params_head;
      adv(ctx);

      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        if (!fnode->as.func_def.is_extern) {
          report_error(
              ctx, ctx->curr,
              "Error: Method '%.*s' must be marked 'extern' if no body at "
              "line %u, col %u\n",
              name.len, name.start, ctx->lex->line, ctx->lex->col);
          return false;
        }
        adv(ctx);
        fnode->as.func_def.block = NULL;
        fnode->src_end = ctx->prev.start + ctx->prev.len;
        AstNode **target_list = (parent->type == AST_STRUCT)
                                    ? &parent->as.struct_def.contents
                                    : &parent->as.union_def.contents;
        append_stmt(target_list, fnode);
        push_state(ctx, current_state);
        break;
      } else {
        // Push method node and let STATE_IN_FUNC parse the body
        AstNode **target_list = (parent->type == AST_STRUCT)
                                    ? &parent->as.struct_def.contents
                                    : &parent->as.union_def.contents;
        append_stmt(target_list, fnode);
        push_node(ctx, fnode);
        push_state(ctx, current_state);
        push_state(ctx, STATE_IN_FUNC);
        break;
      }
    } else {
      // Field declaration
      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        adv(ctx);
      } else {
        report_error(ctx, ctx->curr, "Expected ';' after field declaration");
        adv(ctx);
        sync(ctx);
        recover_state(ctx, current_state);
        break;
      }
      AstNode *field_node = new_node(ctx->arena, AST_VAR_DECL);
      field_node->as.var_decl.type = field_type;
      field_node->as.var_decl.id = name;
      AstNode **target_list = (parent->type == AST_STRUCT)
                                  ? &parent->as.struct_def.contents
                                  : &parent->as.union_def.contents;
      append_stmt(target_list, field_node);
      push_state(ctx, current_state);
      break;
    }
  }
  case STATE_IN_ENUM_DEF: {
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      ctx->ag_depth--;
      pop_node(ctx);
      break;
    }

    // Check for nested type definitions
    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "struct", 6) == 0 ||
          strncmp(ctx->curr.start, "union", 5) == 0 ||
          strncmp(ctx->curr.start, "enum", 4) == 0) {
        Token type_kw = ctx->curr;
        adv(ctx);
        AstNode *nested_type = NULL;
        if (strncmp(type_kw.start, "struct", 6) == 0) {
          nested_type = new_node(ctx->arena, AST_STRUCT);
          nested_type->as.struct_def.structn = ctx->curr;
          adv(ctx);
        } else if (strncmp(type_kw.start, "union", 5) == 0) {
          nested_type = new_node(ctx->arena, AST_UNION);
          nested_type->as.union_def.unionn = ctx->curr;
          adv(ctx);
        } else {
          nested_type = new_node(ctx->arena, AST_ENUM);
          nested_type->as.enum_def.enumn = ctx->curr;
          adv(ctx);
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
          ctx->ag_depth++;
        } else {
          report_error(ctx, ctx->curr, "Expected '{' after nested type name");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        // Append to enum contents
        if (parent->as.enum_def.contents == NULL) {
          parent->as.enum_def.contents = nested_type;
        } else {
          AstNode *tail = parent->as.enum_def.contents;
          while (tail->next != NULL)
            tail = tail->next;
          tail->next = nested_type;
        }
        push_node(ctx, nested_type);
        push_state(ctx, current_state);
        push_state(ctx, (nested_type->type == AST_STRUCT)  ? STATE_IN_STRUCT_DEF
                        : (nested_type->type == AST_UNION) ? STATE_IN_UNION_DEF
                                                           : STATE_IN_ENUM_DEF);
        break;
      }
    }

    if (ctx->curr.type != TOKEN_IDENTIF) {
      report_error(ctx, ctx->curr, "Expected identifier in enum");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    AstNode *enum_member = new_node(ctx->arena, AST_ENUM_MEMBER);
    enum_member->as.enum_member.name = ctx->curr;
    adv(ctx);

    if (parent->as.enum_def.contents == NULL) {
      parent->as.enum_def.contents = enum_member;
    } else {
      AstNode *tail = parent->as.enum_def.contents;
      while (tail->next != NULL)
        tail = tail->next;
      tail->next = enum_member;
    }

    if (ctx->curr.type == TOKEN_ASSIGN) {
      adv(ctx);
      push_state(ctx, STATE_IN_ENUM_DEF);
      push_state(ctx, STATE_ENUM_MEMBER_DONE);
      push_node(ctx, enum_member);
      ctx->expect_operand = true;
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    }

    push_state(ctx, current_state);
    break;
  }

  case STATE_ENUM_MEMBER_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *enum_member = pop_node(ctx);

    enum_member->as.enum_member.val = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    }
    break;
  }
  case STATE_RET_DONE: {
    AstNode *expr_result = pop_node(ctx);
    AstNode *ret_node = pop_node(ctx);

    ret_node->as.ret_stmt.expr = expr_result;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else if (expr_result->type != AST_BLOCK) {
      fprintf(
          stderr,
          "Error: Expected ';' after return expression at line %u, col %u\n",
          ctx->lex->line, ctx->lex->col);
      return false;
    }
    break;
  }
  case STATE_VAR_INIT_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *var_node = ctx->node_stack[ctx->node_count - 1];

    var_node->as.var_decl.init = init_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ';' after variable declaration");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    pop_node(ctx);
    break;
  }
  case STATE_IN_FUNC_ARGS: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
      if (ctx->op_count > 0 &&
          *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
        ctx->op_count--;
      }
      ctx->expect_operand = false;
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
        adv(ctx);
        if (ctx->op_count > 0 &&
            *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
          ctx->op_count--;
        }
        ctx->expect_operand = false;
        push_state(ctx, STATE_IN_EXPR);
        break;
      }
    }

    if (ctx->curr.type == TOKEN_EOF ||
        (ctx->curr.type == TOKEN_PUNC &&
         (*ctx->curr.start == ';' || *ctx->curr.start == '}'))) {
      report_error(ctx, ctx->curr, "Unexpected token in function arguments");
      recover_state(ctx, current_state);
      break;
    }

    push_state(ctx, STATE_IN_FUNC_ARGS);
    push_state(ctx, STATE_ARG_DONE);
    push_state(ctx, STATE_IN_EXPR);
    break;
  }

  case STATE_ARG_DONE: {
    AstNode *arg_expr = pop_node(ctx);
    AstNode *call_node = ctx->node_stack[ctx->node_count - 1];

    if (call_node->as.func_call.args == NULL) {
      call_node->as.func_call.args = arg_expr;
    } else {
      AstNode *tail = call_node->as.func_call.args;
      while (tail->next)
        tail = tail->next;
      tail->next = arg_expr;
    }
    break;
  }
  case STATE_SWITCH_COND_DONE: {
    AstNode *cond_expr = pop_node(ctx);
    AstNode *switch_node = ctx->node_stack[ctx->node_count - 1];
    switch_node->as.switch_stmt.check = cond_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after switch condition");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected '{' to start switch body");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_PARSE_SWITCH_BODY: {
    AstNode *switch_node = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "case", 4) == 0) {
        adv(ctx);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '(' after case");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }

        AstNode *case_node = new_node(ctx->arena, AST_CASE);

        if (switch_node->as.switch_stmt.cases == NULL) {
          switch_node->as.switch_stmt.cases = case_node;
        } else {
          AstNode *tail = switch_node->as.switch_stmt.cases;
          while (tail->next != NULL)
            tail = tail->next;
          tail->next = case_node;
        }

        push_node(ctx, case_node);
        push_state(ctx, STATE_PARSE_SWITCH_BODY);
        push_state(ctx, STATE_CASE_BODY_DONE);
        push_state(ctx, STATE_CASE_EXPR_DONE);
        push_state(ctx, STATE_IN_EXPR);
        break;
      } else if (strncmp(ctx->curr.start, "default", 7) == 0) {
        adv(ctx);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          AstNode *default_block = new_node(ctx->arena, AST_BLOCK);
          switch_node->as.switch_stmt.default_case = default_block;

          push_state(ctx, STATE_PARSE_SWITCH_BODY);
          push_state(ctx, STATE_BLOCK_DONE);
          push_node(ctx, default_block);
          push_state(ctx, STATE_PARSE_BLOCK);
          adv(ctx);
        } else {
          report_error(ctx, ctx->curr, "Expected '{' for default body");
          adv(ctx);
          sync(ctx);
          recover_state(ctx, current_state);
          break;
        }
        break;
      }
    }

    report_error(ctx, ctx->curr, "Unexpected token in switch body");
    adv(ctx);
    sync(ctx);
    recover_state(ctx, current_state);
    break;
  }

  case STATE_CASE_EXPR_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *case_node = ctx->node_stack[ctx->node_count - 1];
    case_node->as.case_stmt.val = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected ')' after case expression");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      AstNode *case_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, case_block);
      push_state(ctx, STATE_PARSE_BLOCK);
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr, "Expected '{' to start case body");
      adv(ctx);
      sync(ctx);
      recover_state(ctx, current_state);
      break;
    }
    break;
  }

  case STATE_CASE_BODY_DONE: {
    AstNode *case_block = pop_node(ctx);
    AstNode *case_node = pop_node(ctx);
    case_node->as.case_stmt.action = case_block;
    break;
  }

  case STATE_SWITCH_DONE: {
    pop_node(ctx);
    break;
  }
  }
  return true;
}

DataType parse_type(ParseCtx *ctx) {
  DataType type = {0};

  while (true) {
    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "static", ctx->curr.len) == 0) {
        type.is_static = true;
        adv(ctx);
      } else if (strncmp(ctx->curr.start, "mut", ctx->curr.len) == 0) {
        type.is_mut = true;
        adv(ctx);
      } else if (strncmp(ctx->curr.start, "async", ctx->curr.len) == 0) {
        type.is_async = true;
        adv(ctx);
      } else if (strncmp(ctx->curr.start, "inline", ctx->curr.len) == 0) {
        type.is_inline = true;
        adv(ctx);
      } else if (strncmp(ctx->curr.start, "threadlocal", ctx->curr.len) == 0) {
        type.is_threadlocal = true;
        adv(ctx);
      } else if (strncmp(ctx->curr.start, "extern", ctx->curr.len) == 0) {
        type.is_extern = true;
        adv(ctx);
      } else {
        break;
      }
    } else if (ctx->curr.len == 1 &&
               (*ctx->curr.start == '*' || *ctx->curr.start == '&')) {
      switch (*ctx->curr.start) {
      case '*': {
        type.ptr_depth++;
        break;
      }
      case '&': {
        type.ptr_depth--;
        break;
      }
      }
      adv(ctx);
    } else {
      break;
    }
  }

  while (ctx->curr.len == 1 &&
         (*ctx->curr.start == '*' || *ctx->curr.start == '&')) {
    switch (*ctx->curr.start) {
    case '*': {
      type.ptr_depth++;
      break;
    }
    case '&': {
      type.ptr_depth--;
      break;
    }
    }
    adv(ctx);
  }

  if (ctx->curr.type == TOKEN_IDENTIF && ctx->curr.len == 4 &&
      strncmp(ctx->curr.start, "self", 4) == 0) {
    if (ctx->ag_depth == 0) {
      report_error(
          ctx, ctx->curr,
          "Error: 'self' type can only be used inside struct/union/enum at "
          "line %u, col %u\n",
          ctx->lex->line, ctx->lex->col);
      // Return a dummy type to avoid crashing
      type.name = ctx->curr;
      adv(ctx);
      return type;
    }
    type.is_self = true;

    // Find the parent sue type to inherit its actual name
    for (int i = ctx->node_count - 1; i >= 0; i--) {
      AstNode *parent = ctx->node_stack[i];
      if (parent->type == AST_STRUCT) {
        type.name = parent->as.struct_def.structn;
        break;
      } else if (parent->type == AST_UNION) {
        type.name = parent->as.union_def.unionn;
        break;
      } else if (parent->type == AST_ENUM) {
        type.name = parent->as.enum_def.enumn;
        break;
      }
    }

    adv(ctx);
  } else if (ctx->curr.type == TOKEN_IDENTIF ||
             is_builtin_type_kw(ctx, ctx->curr)) {
    type.name = ctx->curr;
    if (ctx->curr.type == TOKEN_IDENTIF)
      type.is_custom = true;
    adv(ctx);

    if (type.is_custom && ctx->curr.len == 1 && *ctx->curr.start == '.') {
      adv(ctx);

      if (ctx->curr.type == TOKEN_IDENTIF) {
        type.name.len = (ctx->curr.start - type.name.start) + ctx->curr.len;
        adv(ctx);
      } else {
        report_error(ctx, ctx->curr, "Expected identifier after '.' in type");
      }
    }
  } else {
    report_error(ctx, ctx->curr, "Expected type name at line %u, col %u\n",
                 ctx->lex->line, ctx->lex->col);
  }

  while (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '[') {
    adv(ctx);
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      type.array_dimens++;
      continue;
    }

    ctx->expect_operand = true;
    push_state(ctx, STATE_IN_EXPR);

    size_t target_state = ctx->state_count - 1;
    while (ctx->state_count > target_state && ctx->curr.type != TOKEN_EOF) {
      if (!parse_step(ctx))
        return type;
    }

    AstNode *expr_node = pop_node(ctx);

    if (type.dim_sizes == NULL) {
      type.dim_sizes = arena_alloc(ctx->arena, sizeof(AstNode *) * 8);
    }

    type.dim_sizes[type.array_dimens] = expr_node;
    type.array_dimens++;
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
    } else {
      report_error(ctx, ctx->curr,
                   "Expected ']' after array dimension at line %u, col %u\n",
                   ctx->lex->line, ctx->lex->col);
    }
  }

  return type;
}

bool parse(ParseCtx *ctx) {
  push_state(ctx, STATE_GLOBAL);

  while (ctx->state_count > 0 && ctx->curr.type != TOKEN_EOF) {
    parse_step(ctx);

    if (ctx->curr.type == TOKEN_PUNC &&
        (*ctx->curr.start == ';' || *ctx->curr.start == '}')) {
      ctx->panic_mode = false;
    }
  }

  return ctx->err_count == 0;
}

inline bool is_kw(LexCtx *ctx, const char *start, unsigned int len) {
  return map_get(&ctx->kw_map, start, len) != NULL;
}

inline bool is_op(LexCtx *ctx, const char *start, unsigned int len) {
  return map_get(&ctx->op_map, start, len) != NULL;
}

inline bool is_compare(LexCtx *ctx, const char *start, unsigned int len) {
  return map_get(&ctx->comp_map, start, len) != NULL;
}

inline bool is_punc(char c) {
  switch (c) {
  case ',':
  case '{':
  case '}':
  case '(':
  case ')':
  case '[':
  case ']':
  case ';':
    return true;
  default:
    return false;
  }
}

inline bool is_numeric_slice(const char *start, unsigned int len) {
  if (len == 0)
    return false;

  unsigned int i = 0;
  bool has_decimal = false;
  bool has_digits = false;

  if (start[0] == '-') {
    i++;
  }

  for (; i < len; i++) {
    if (start[i] == '.') {
      if (has_decimal)
        return false;
      has_decimal = true;
    } else if (start[i] >= '0' && start[i] <= '9') {
      has_digits = true;
    } else {
      return false;
    }
  }

  return has_digits;
}

bool is_lit(const char *start, unsigned int len) {
  if (len == 0)
    return false;

  if (len >= 2 &&
      ((start[0] == '"' && start[len - 1] == '"') ||
       (start[0] == '\'' && start[len - 1] == '\'')) &&
      start[len - 2] == '\\') {
    return true;
  }

  if ((len == 4 && strncmp(start, "true", 4) == 0) ||
      (len == 5 && strncmp(start, "false", 5) == 0)) {
    return true;
  }

  return is_numeric_slice(start, len);
}
