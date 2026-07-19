#include "fmt.h"
#include "arena.h"
#include "ast_types.h"
#include "ast_visitor.h"
#include "diag.h"
#include "fmt.h"
#include "hashmap.h"
#include "util.h"
#include "worklist.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  HashMap *type_set;
  Arena *arena;
} CollectTypeData;

VisitResult collect_type_enter(AstVisitor *visitor, AstNode *node) {
  CollectTypeData *data = (CollectTypeData *)visitor->user_data;
  Token name = {0};

  if (node->type == AST_STRUCT) {
    name = node->as.struct_def.structn;
  } else if (node->type == AST_UNION) {
    name = node->as.union_def.unionn;
  } else if (node->type == AST_ENUM) {
    name = node->as.enum_def.enumn;
  }

  if (name.len > 0) {
    char *type_name = arena_alloc(data->arena, name.len + 1);
    memcpy(type_name, name.start, name.len);
    type_name[name.len] = '\0';
    map_set(data->type_set, type_name, name.len, (void *)(uintptr_t)1);
  }

  return VISIT_CONTINUE;
}

void collect_type_names(AstNode *root, HashMap *type_set, Arena *arena) {
  if (!root)
    return;

  CollectTypeData data = {type_set, arena};
  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = collect_type_enter;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered whilst collecting type names.\n");
  }
}

bool is_builtin_type_name(const char *start, size_t len) {
  for (size_t i = 0; i < typelistlen; i++) {
    if (strlen(typelist[i]) == len && strncmp(typelist[i], start, len) == 0)
      return true;
  }
  return false;
}

char *format_identifier(const char *input, size_t len, FormatStyle style) {
  if (len == 0 || !input)
    return NULL;

  char *output = calloc(1, len * 2 + 1);
  size_t out_idx = 0;
  size_t word_start = 0;
  int word_count = 0;

  const char *known_acronyms[] = {
      "id",   "ast",  "xml", "url", "html", "http", "https", "tcp",
      "udp",  "ip",   "dns", "ssl", "tls",  "json", "api",   "csv",
      "uri",  "css",  "dom", "sql", "rpc",  "ssh",  "ftp",   "mac",
      "jwt",  "cpu",  "gpu", "io",  "os",   "db",   "cli",   "sdk",
      "uuid", "guid", "ir",  "cfg", "eof",  NULL};

  for (size_t i = 0; i <= len; i++) {
    bool is_end = (i == len);
    bool is_underscore = (!is_end && input[i] == '_');
    bool is_camel_boundary =
        (!is_end && i > 0 && islower(input[i - 1]) && isupper(input[i]));
    bool is_acronym_boundary =
        (!is_end && i > 1 && isupper(input[i - 1]) && isupper(input[i]) &&
         i + 1 < len && islower(input[i + 1]));

    if (is_end || is_underscore || is_camel_boundary || is_acronym_boundary) {
      size_t word_len = i - word_start;

      if (word_len > 0) {
        bool is_acronym = true;
        for (size_t w = 0; w < word_len; w++) {
          if (islower(input[word_start + w])) {
            is_acronym = false;
            break;
          }
        }

        if (!is_acronym) {
          char *lower_word = malloc(word_len + 1);
          if (lower_word) {
            for (size_t w = 0; w < word_len; w++) {
              lower_word[w] = tolower(input[word_start + w]);
            }
            lower_word[word_len] = '\0';

            for (const char **ac = known_acronyms; *ac; ac++) {
              if (strcmp(lower_word, *ac) == 0) {
                is_acronym = true;
                break;
              }
            }
            free(lower_word);
          }
        }

        if (style == FMT_SNAKE_CASE && word_count > 0) {
          output[out_idx++] = '_';
        }

        for (size_t w = 0; w < word_len; w++) {
          char c = input[word_start + w];
          if (style == FMT_SNAKE_CASE) {
            output[out_idx++] = tolower(c);
          } else if (style == FMT_CAMEL_CASE && word_count == 0) {
            output[out_idx++] = tolower(c);
          } else {
            if (is_acronym) {
              output[out_idx++] = toupper(c);
            } else {
              output[out_idx++] = (w == 0) ? toupper(c) : tolower(c);
            }
          }
        }
        word_count++;
      }

      if (is_underscore) {
        word_start = i + 1;
      } else if (is_camel_boundary || is_acronym_boundary) {
        word_start = i;
      } else if (is_end) {
      } else {
        word_start = i;
      }
    }
  }

  return output;
}

void print_comments_between(const char *start, const char *end, FILE *out_fp,
                            unsigned int depth) {
  if (!start || !end || start >= end)
    return;
  const char *p = start;

  while (p < end) {
    if (p[0] == '/' && p[1] == '/') {
      // Check if this comment is on a new line or trailing the previous node
      bool is_new_line = false;
      const char *check = p - 1;
      while (check >= start) {
        if (*check == '\n') {
          is_new_line = true;
          break;
        }
        if (!isspace((unsigned char)*check)) {
          is_new_line = false;
          break;
        }
        check--;
      }
      if (check < start)
        is_new_line = false;

      if (is_new_line) {
        for (unsigned int i = 0; i < depth; i++)
          fputc('\t', out_fp);
      } else {
        fputc(' ', out_fp); // Preserve inline spacing for trailing comments
      }

      // Print the comment content up to the newline
      while (p < end && *p != '\n' && *p != '\r') {
        fputc(*p, out_fp);
        p++;
      }
      fputc('\n', out_fp);
    } else {
      p++;
    }
  }
}

typedef struct {
  FILE *out_fp;
  HashMap *type_set;
  const char *last_pos;
  unsigned int depth;

  AstNode **parent_stack;
  int *interleave_counts;
  size_t parent_cap;
  size_t parent_top;

  bool oom;
} FmtData;

#define FPRINTF_SAFE(...)                                                      \
  do {                                                                         \
    if (fprintf(data->out_fp, __VA_ARGS__) < 0) {                              \
      goto err_cleanup;                                                        \
    }                                                                          \
  } while (0)

#define FMT_TYPE_SAFE(t)                                                       \
  do {                                                                         \
    if ((t).is_static)                                                         \
      FPRINTF_SAFE("%s", "static ");                                           \
    if ((t).is_threadlocal)                                                    \
      FPRINTF_SAFE("%s", "threadlocal ");                                      \
    if ((t).is_extern)                                                         \
      FPRINTF_SAFE("%s", "extern ");                                           \
    if ((t).is_async)                                                          \
      FPRINTF_SAFE("%s", "async ");                                            \
    if ((t).is_inline)                                                         \
      FPRINTF_SAFE("%s", "inline ");                                           \
    if ((t).ptr_depth != 0) {                                                  \
      char sym = ((t).ptr_depth > 0) ? '*' : '&';                              \
      int cnt = ((t).ptr_depth > 0) ? (t).ptr_depth : -(t).ptr_depth;          \
      for (int _i = 0; _i < cnt; _i++)                                         \
        FPRINTF_SAFE("%c", sym);                                               \
    }                                                                          \
    if ((t).is_mut)                                                            \
      FPRINTF_SAFE("%s", "mut ");                                              \
    if ((t).is_self) {                                                         \
      FPRINTF_SAFE("%s", "self");                                              \
    } else if ((t).name.len > 0) {                                             \
      FPRINTF_SAFE("%.*s", (int)(t).name.len, (t).name.start);                 \
    }                                                                          \
    for (unsigned int _i = 0; _i < (t).array_dimens; _i++) {                   \
      if ((t).dim_sizes && _i < (t).array_dimens && (t).dim_sizes[_i]) {       \
        AstNode *dim = (t).dim_sizes[_i];                                      \
        if (dim->type == AST_NUM_LIT) {                                        \
          FPRINTF_SAFE("[%.*s]", (int)dim->as.num_lit.val.len,                 \
                       dim->as.num_lit.val.start);                             \
        } else {                                                               \
          FPRINTF_SAFE("%s", "[expr]");                                        \
        }                                                                      \
      } else {                                                                 \
        FPRINTF_SAFE("%s", "[]");                                              \
      }                                                                        \
    }                                                                          \
  } while (0)

// Check if a newline or semicolon is needed
bool is_statement_context(AstNode *node, AstNode *parent) {
  if (!parent)
    return true;
  switch (parent->type) {
  case AST_PROGRAM:
  case AST_BLOCK:
  case AST_EXTERN:
    return true;
  case AST_IF:
    return node == parent->as.if_check.action ||
           node == parent->as.if_check.elseAct;
  case AST_WHILE:
    return node == parent->as.while_loop.action;
  case AST_FOR:
    return node == parent->as.for_loop.action;
  case AST_CASE:
    return node == parent->as.case_stmt.action;
  default:
    return false;
  }
}

// Check if a node does not print its own semicolon
bool needs_semi(AstNode *stmt) {
  switch (stmt->type) {
  case AST_FUNC_CALL:
  case AST_BINOP:
  case AST_UOP:
  case AST_IDENTIF:
  case AST_NUM_LIT:
  case AST_STR_LIT:
  case AST_CHAR_LIT:
  case AST_BOOL_LIT:
  case AST_NULL_LIT:
  case AST_MEMBER:
  case AST_ARRAY_LIT:
  case AST_INDEX:
  case AST_CAST:
  case AST_SIZEOF:
    return true;
  default:
    return false;
  }
}

VisitResult fmt_enter_node(AstVisitor *visitor, AstNode *node) {
  FmtData *data = (FmtData *)visitor->user_data;

  if (node->src_start && data->last_pos < node->src_start) {
    print_comments_between(data->last_pos, node->src_start, data->out_fp,
                           data->depth);
    data->last_pos = node->src_start;
  }

  AstNode *parent =
      data->parent_top > 0 ? data->parent_stack[data->parent_top - 1] : NULL;

  // Track stack
  if (data->parent_top >= data->parent_cap) {
    data->parent_cap = data->parent_cap == 0 ? 128 : data->parent_cap * 2;
    data->parent_stack =
        realloc(data->parent_stack, data->parent_cap * sizeof(AstNode *));
    data->interleave_counts =
        realloc(data->interleave_counts, data->parent_cap * sizeof(int));
  }
  data->parent_stack[data->parent_top] = node;
  data->interleave_counts[data->parent_top] = 0;
  data->parent_top++;

  // Base indentation
  if (parent) {
    bool indent = false;
    if (parent->type == AST_PROGRAM || parent->type == AST_BLOCK ||
        parent->type == AST_EXTERN)
      indent = true;
    else if (parent->type == AST_STRUCT || parent->type == AST_UNION ||
             parent->type == AST_ENUM)
      indent = true;
    else if (parent->type == AST_SWITCH &&
             (node->type == AST_CASE ||
              node == parent->as.switch_stmt.default_case))
      indent = true;

    if (indent) {
      for (unsigned int i = 0; i < data->depth; i++)
        FPRINTF_SAFE("%c", '\t');
    }
  }

  switch (node->type) {
  case AST_BLOCK:
    if (parent && (parent->type == AST_FUNC || parent->type == AST_IF ||
                   parent->type == AST_WHILE || parent->type == AST_FOR ||
                   parent->type == AST_CASE || parent->type == AST_DEFER)) {
      if (parent->type == AST_FUNC)
        FPRINTF_SAFE(") ");
      else
        FPRINTF_SAFE(" ");
    }
    if (parent && parent->type == AST_SWITCH &&
        node == parent->as.switch_stmt.default_case) {
      FPRINTF_SAFE("default ");
    }
    FPRINTF_SAFE("{\n");
    data->depth++;
    break;
  case AST_EXTERN:
    FPRINTF_SAFE("extern {\n");
    data->depth++;
    break;
  case AST_FUNC:
    FMT_TYPE_SAFE(node->as.func_def.ret_type);
    FPRINTF_SAFE(" %.*s(", (int)node->as.func_def.fn_name.len,
                 node->as.func_def.fn_name.start);
    break;
  case AST_PARAM: {
    FMT_TYPE_SAFE(node->as.fn_param.type);
    bool is_implicit_self = node->as.fn_param.type.is_self &&
                            node->as.fn_param.id.len == 4 &&
                            strncmp(node->as.fn_param.id.start, "self", 4) == 0;
    if (!is_implicit_self)
      FPRINTF_SAFE(" %.*s", (int)node->as.fn_param.id.len,
                   node->as.fn_param.id.start);
    break;
  }
  case AST_VAR_DECL:
    FMT_TYPE_SAFE(node->as.var_decl.type);
    FPRINTF_SAFE(" %.*s", (int)node->as.var_decl.id.len,
                 node->as.var_decl.id.start);
    if (node->as.var_decl.init)
      FPRINTF_SAFE(" = ");
    break;
  case AST_STRUCT:
  case AST_UNION:
  case AST_ENUM: {
    const char *kw = (node->type == AST_STRUCT)  ? "struct"
                     : (node->type == AST_UNION) ? "union"
                                                 : "enum";
    Token name_tok = (node->type == AST_STRUCT)  ? node->as.struct_def.structn
                     : (node->type == AST_UNION) ? node->as.union_def.unionn
                                                 : node->as.enum_def.enumn;
    bool is_decl_only = false;

    if (node->src_end && node->src_end > node->src_start &&
        *(node->src_end - 1) == ';') {
      is_decl_only = true;
    }

    bool is_extern_node =
        (node->type == AST_STRUCT && node->as.struct_def.is_extern) ||
        (node->type == AST_UNION && node->as.union_def.is_extern);
    if (is_extern_node)
      FPRINTF_SAFE("extern ");
    FPRINTF_SAFE("%s ", kw);

    if (is_decl_only) {
      FPRINTF_SAFE("%.*s;\n\n", (int)name_tok.len, name_tok.start);
      return VISIT_SKIP_CHILDREN;
    } else {
      FPRINTF_SAFE("%.*s {\n", (int)name_tok.len, name_tok.start);
      data->depth++;
    }
    break;
  }
  case AST_IDENTIF:
    FPRINTF_SAFE("%.*s", (int)node->as.identif.val.len,
                 node->as.identif.val.start);
    break;
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
    if (!node->as.unop.is_postfix)
      FPRINTF_SAFE("%.*s", node->as.unop.op.len, node->as.unop.op.start);
    break;
  case AST_IF:
    FPRINTF_SAFE("if (");
    break;
  case AST_NUM_LIT:
    FPRINTF_SAFE("%.*s", node->as.num_lit.val.len, node->as.num_lit.val.start);
    break;
  case AST_STR_LIT:
    FPRINTF_SAFE("%.*s", node->as.str_lit.val.len, node->as.str_lit.val.start);
    break;
  case AST_CHAR_LIT:
    FPRINTF_SAFE("%.*s", node->as.char_lit.val.len,
                 node->as.char_lit.val.start);
    break;
  case AST_BOOL_LIT:
    FPRINTF_SAFE("%.*s", node->as.bool_lit.val.len,
                 node->as.bool_lit.val.start);
    break;
  case AST_NULL_LIT:
    FPRINTF_SAFE("null");
    break;
  case AST_RET:
    FPRINTF_SAFE("ret");
    if (node->as.ret_stmt.expr)
      FPRINTF_SAFE(" ");
    break;
  case AST_ARRAY_LIT:
    FPRINTF_SAFE("[");
    break;
  case AST_ENUM_MEMBER:
    FPRINTF_SAFE("%.*s", (int)node->as.enum_member.name.len,
                 node->as.enum_member.name.start);
    if (node->as.enum_member.val)
      FPRINTF_SAFE(" = ");
    break;
  case AST_DEFER:
    if (node->as.defer_stmt.contents &&
        node->as.defer_stmt.contents->type == AST_BLOCK)
      FPRINTF_SAFE("defer");
    else
      FPRINTF_SAFE("defer ");
    break;
  case AST_FOR:
    FPRINTF_SAFE("for (");
    break;
  case AST_WHILE:
    FPRINTF_SAFE("while (");
    break;
  case AST_SWITCH:
    FPRINTF_SAFE("switch (");
    break;
  case AST_CASE:
    if (node->as.case_stmt.val == NULL) {
      FPRINTF_SAFE("default");
    } else {
      FPRINTF_SAFE("case (");
    }
    break;
  case AST_USE:
    FPRINTF_SAFE("use %.*s", (int)node->as.use_stmt.path.len,
                 node->as.use_stmt.path.start);
    if (node->as.use_stmt.alias.len > 0)
      FPRINTF_SAFE(" as %.*s", (int)node->as.use_stmt.alias.len,
                   node->as.use_stmt.alias.start);
    FPRINTF_SAFE(";\n");
    if (node->next && node->next->type != AST_USE)
      FPRINTF_SAFE("\n");
    break;
  case AST_BREAK:
    FPRINTF_SAFE("break;\n");
    break;
  case AST_CONTINUE:
    FPRINTF_SAFE("continue;\n");
    break;
  case AST_CAST:
    FPRINTF_SAFE("(");
    if (node->as.cast.target.is_self)
      FPRINTF_SAFE("self");
    else
      FPRINTF_SAFE("%.*s", (int)node->as.cast.target.name.len,
                   node->as.cast.target.name.start);
    FPRINTF_SAFE(")");
    break;
  case AST_SIZEOF:
    if (!node->as.sizeof_expr.is_type && node->as.sizeof_expr.target_expr &&
        node->as.sizeof_expr.target_expr->type == AST_CAST) {
      AstNode *cast = node->as.sizeof_expr.target_expr;
      AstNode *op = cast->as.cast.op;
      if (op && op->type == AST_IDENTIF &&
          map_get(data->type_set, op->as.identif.val.start,
                  op->as.identif.val.len) != NULL) {
        FPRINTF_SAFE("(");
        FMT_TYPE_SAFE(cast->as.cast.target);
        FPRINTF_SAFE(")sizeof(%.*s)", (int)op->as.identif.val.len,
                     op->as.identif.val.start);
        return VISIT_SKIP_CHILDREN;
      }
    }
    FPRINTF_SAFE("sizeof(");
    if (node->as.sizeof_expr.is_type) {
      if (node->as.sizeof_expr.target_type.is_self)
        FPRINTF_SAFE("self");
      else
        FPRINTF_SAFE("%.*s", (int)node->as.sizeof_expr.target_type.name.len,
                     node->as.sizeof_expr.target_type.name.start);
      FPRINTF_SAFE(")");
    }
    break;
  default:
    break;
  }

  // Handle gap prints before specific statements for `for` loops
  if (parent && parent->type == AST_FOR) {
    if (node == parent->as.for_loop.check) {
      if (!parent->as.for_loop.init)
        FPRINTF_SAFE("; ");
    } else if (node == parent->as.for_loop.inc) {
      if (!parent->as.for_loop.init && !parent->as.for_loop.check)
        FPRINTF_SAFE("; ; ");
      else if (!parent->as.for_loop.check)
        FPRINTF_SAFE("; ");
    } else if (node == parent->as.for_loop.action) {
      if (!parent->as.for_loop.init && !parent->as.for_loop.check &&
          !parent->as.for_loop.inc)
        FPRINTF_SAFE("; ; ) ");
      else if (!parent->as.for_loop.check && !parent->as.for_loop.inc)
        FPRINTF_SAFE("; ) ");
      else if (!parent->as.for_loop.inc)
        FPRINTF_SAFE(") ");
    }
  }

  return VISIT_CONTINUE;

err_cleanup:
  data->oom = true;
  return VISIT_ABORT;
}

void fmt_interleave_node(AstVisitor *visitor, AstNode *node, int step) {
  (void)step; // Unused
  FmtData *data = (FmtData *)visitor->user_data;

  // Node here is parent currently at top of stack
  int count = data->interleave_counts[data->parent_top - 1]++;

  switch (node->type) {
  case AST_FUNC:
    FPRINTF_SAFE(", ");
    break;
  case AST_BINOP:
    FPRINTF_SAFE(" %.*s ", (int)node->as.binop.op.len, node->as.binop.op.start);
    break;
  case AST_IF:
    if (count == 0) {
      if (node->as.if_check.action &&
          node->as.if_check.action->type == AST_BLOCK)
        FPRINTF_SAFE(")");
      else
        FPRINTF_SAFE(") ");
    } else if (count == 1) {
      if (node->as.if_check.elseAct &&
          node->as.if_check.elseAct->type == AST_BLOCK)
        FPRINTF_SAFE(" else");
      else
        FPRINTF_SAFE(" else ");
    }
    break;
  case AST_FUNC_CALL:
    if (count == 0)
      FPRINTF_SAFE("(");
    else
      FPRINTF_SAFE(", ");
    break;
  case AST_ARRAY_LIT:
    FPRINTF_SAFE(", ");
    break;
  case AST_INDEX:
    if (count == 0)
      FPRINTF_SAFE("[");
    break;
  default:
    break;
  }
  return;
err_cleanup:
  data->oom = true;
}

void fmt_exit_node(AstVisitor *visitor, AstNode *node) {
  FmtData *data = (FmtData *)visitor->user_data;
  AstNode *parent =
      data->parent_top > 1 ? data->parent_stack[data->parent_top - 2] : NULL;

  switch (node->type) {
  case AST_BLOCK:
    data->depth--;
    for (unsigned int i = 0; i < data->depth; i++)
      FPRINTF_SAFE("%c", '\t');
    FPRINTF_SAFE("}");
    if (parent && parent->type == AST_EXTERN)
      FPRINTF_SAFE("\n\n");
    else if (parent && parent->type == AST_SWITCH && 
             node == parent->as.switch_stmt.default_case)
        FPRINTF_SAFE("\n");
    break;
  case AST_EXTERN:
    data->depth--;
    for (unsigned int i = 0; i < data->depth; i++)
      FPRINTF_SAFE("%c", '\t');
    FPRINTF_SAFE("}\n\n");
    break;
  case AST_FUNC:
    if (!node->as.func_def.block)
      FPRINTF_SAFE(");\n");
    else {
      FPRINTF_SAFE("\n");
      if (node->next)
        FPRINTF_SAFE("\n");
    }
    break;
  case AST_STRUCT:
  case AST_UNION:
  case AST_ENUM: {
    bool is_decl_only = false;
    if (node->src_end && node->src_end > node->src_start &&
        *(node->src_end - 1) == ';')
      is_decl_only = true;
    if (!is_decl_only) {
      data->depth--;
      for (unsigned int i = 0; i < data->depth; i++)
        FPRINTF_SAFE("%c", '\t');
      FPRINTF_SAFE("}\n\n");
    }
    break;
  }
  case AST_FUNC_CALL:
    FPRINTF_SAFE(")");
    break;
  case AST_ARRAY_LIT:
    FPRINTF_SAFE("]");
    break;
  case AST_ENUM_MEMBER:
    FPRINTF_SAFE(",\n");
    break;
  case AST_INDEX:
    FPRINTF_SAFE("]");
    break;
  case AST_MEMBER:
    FPRINTF_SAFE(".%.*s", (int)node->as.member.name.len,
                 node->as.member.name.start);
    break;
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
    if (node->as.unop.is_postfix)
      FPRINTF_SAFE("%.*s", node->as.unop.op.len, node->as.unop.op.start);
    break;
  case AST_SIZEOF:
    if (!(!node->as.sizeof_expr.is_type && node->as.sizeof_expr.target_expr &&
          node->as.sizeof_expr.target_expr->type == AST_CAST)) {
      if (!node->as.sizeof_expr.is_type)
        FPRINTF_SAFE(")");
    }
    break;
  case AST_VAR_DECL: {
    bool is_for_init =
        parent && parent->type == AST_FOR && node == parent->as.for_loop.init;
    if (is_for_init)
      FPRINTF_SAFE("; ");
    else {
      FPRINTF_SAFE(";\n");
      bool is_global = !parent || parent->type == AST_PROGRAM ||
                       parent->type == AST_STRUCT ||
                       parent->type == AST_UNION || parent->type == AST_EXTERN;
      if (is_global && node->next && node->next->type != AST_VAR_DECL) {
        FPRINTF_SAFE("\n");
      } else if (!is_global && node->next && node->src_end &&
                 node->next->src_start) {
        int nl_count = 0;
        for (const char *p = node->src_end; p < node->next->src_start; p++) {
          if (*p == '\n')
            nl_count++;
        }
        if (nl_count >= 2)
          FPRINTF_SAFE("\n");
      }
    }
    break;
  }
  case AST_RET:
  case AST_DEFER:
    if (node->type == AST_DEFER && node->as.defer_stmt.contents &&
        node->as.defer_stmt.contents->type == AST_BLOCK)
      FPRINTF_SAFE("\n");
    else
      FPRINTF_SAFE(";\n");
    break;
  case AST_SWITCH:
    data->depth--;
    for (unsigned int i = 0; i < data->depth; i++)
      FPRINTF_SAFE("%c", '\t');
    FPRINTF_SAFE("}\n");
    break;
  case AST_CASE:
    if (node->as.case_stmt.action &&
        node->as.case_stmt.action->type == AST_BLOCK)
      FPRINTF_SAFE("\n");
    else
      FPRINTF_SAFE(";\n");
    break;
  case AST_IF:
    if (node->as.if_check.elseAct) {
      if (node->as.if_check.elseAct->type == AST_BLOCK)
        FPRINTF_SAFE("\n");
    } else if (node->as.if_check.action->type == AST_BLOCK)
      FPRINTF_SAFE("\n");
    break;
  case AST_WHILE:
    if (node == parent->as.while_loop.check) {
      if (parent->as.while_loop.action &&
          parent->as.while_loop.action->type == AST_BLOCK)
        FPRINTF_SAFE(")");
      else
        FPRINTF_SAFE(") ");
    }
    break;
  case AST_FOR:
    if (!node->as.for_loop.action) {
      if (!node->as.for_loop.init && !node->as.for_loop.check &&
          !node->as.for_loop.inc)
        FPRINTF_SAFE("; ; ) ;\n");
      else if (!node->as.for_loop.check && !node->as.for_loop.inc)
        FPRINTF_SAFE("; ) ;\n");
      else if (!node->as.for_loop.inc)
        FPRINTF_SAFE(") ;\n");
      else
        FPRINTF_SAFE(";\n");
    } else if (node->as.for_loop.action->type == AST_BLOCK)
      FPRINTF_SAFE("\n");
    break;
  default:
    break;
  }

  // Append context based gaps safely
  if (parent) {
    if (parent->type == AST_WHILE && node == parent->as.while_loop.check) {
      FPRINTF_SAFE(") ");
    } else if (parent->type == AST_FOR) {
      if (node == parent->as.for_loop.init) {
        if (node->type != AST_VAR_DECL)
          FPRINTF_SAFE("; ");
      } else if (node == parent->as.for_loop.check) {
        FPRINTF_SAFE("; ");
      } else if (node == parent->as.for_loop.inc) {
        if (parent->as.for_loop.action &&
            parent->as.for_loop.action->type == AST_BLOCK)
          FPRINTF_SAFE(")");
        else
          FPRINTF_SAFE(") ");
      }
    } else if (parent->type == AST_CASE && node == parent->as.case_stmt.val) {
      if (parent->as.case_stmt.action &&
          parent->as.case_stmt.action->type == AST_BLOCK)
        FPRINTF_SAFE(")");
      else
        FPRINTF_SAFE(") ");
    } else if (parent->type == AST_SWITCH &&
               node == parent->as.switch_stmt.check) {
      FPRINTF_SAFE(") {\n");
      data->depth++;
    }
  }

  if (is_statement_context(node, parent) && needs_semi(node)) {
    FPRINTF_SAFE(";\n");
  } else if (node->type == AST_BLOCK && parent &&
             (parent->type == AST_BLOCK || parent->type == AST_PROGRAM ||
              parent->type == AST_EXTERN)) {
    FPRINTF_SAFE("\n");
  }

  if (node->src_end && data->last_pos < node->src_end) {
    data->last_pos = node->src_end;
  }

  data->parent_top--;
  return;

err_cleanup:
  data->oom = true;
}

bool fmt_ast(AstNode *root, FILE *out_fp, HashMap *type_set,
             const char *source_text) {
  if (!root) {
    fprintf(stderr, "No root AST was passed to fmt_ast\n");
    return false;
  }

  FmtData data = {0};
  data.out_fp = out_fp;
  data.type_set = type_set;
  data.last_pos = source_text;
  data.depth = 0;
  data.parent_cap = 128;
  data.parent_stack = malloc(sizeof(AstNode *) * data.parent_cap);
  data.interleave_counts = malloc(sizeof(int) * data.parent_cap);
  data.parent_top = 0;
  data.oom = false;

  if (!data.parent_stack || !data.interleave_counts) {
    fprintf(stderr,
            "OOM encountered whilst allocating formatter tracker stacks.\n");
    if (data.parent_stack)
      free(data.parent_stack);
    if (data.interleave_counts)
      free(data.interleave_counts);
    return false;
  }

  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = fmt_enter_node;
  visitor.interleave_node = fmt_interleave_node;
  visitor.exit_node = fmt_exit_node;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  bool success = true;
  if (setjmp(panic_env) == 0) {
    success = ast_traverse(&visitor, root);
  } else {
    success = false;
  }

  if (data.oom) {
    success = false;
  }

  // Flush trailing comments
  if (success && data.last_pos) {
    size_t remain = strlen(data.last_pos);
    print_comments_between(data.last_pos, data.last_pos + remain, out_fp, 0);
  }

  free(data.parent_stack);
  free(data.interleave_counts);

  return success;
}

#undef FPRINTF_SAFE
#undef FMT_TYPE_SAFE

#ifdef ENABLE_THREADS
#include "thread_pool.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
  Worklist *worklist;
  Arena *global_arena;
  pthread_mutex_t *global_mutex;
  HashMap *seen;
  atomic_size_t *files_in_flight;
  const CompileOptions *opts;
} FmtWorkerData;

bool fmt_worker_loop(void *arg) {
  FmtWorkerData *data = (FmtWorkerData *)arg;
  Worklist *wl = data->worklist;

  while (1) {
    const char *current_path = wl_pop(wl);
    if (!current_path)
      break;

    pthread_mutex_lock(data->global_mutex);
    const char *abs_path = resolve_alloc(data->global_arena, current_path);
    bool already_seen = false;
    if (abs_path) {
      if (map_get(data->seen, abs_path, strlen(abs_path)) != NULL) {
        already_seen = true;
      } else {
        map_set(data->seen, abs_path, strlen(abs_path), (void *)1);
      }
    }
    pthread_mutex_unlock(data->global_mutex);

    if (!abs_path || already_seen) {
      atomic_fetch_sub(data->files_in_flight, 1);
      continue;
    }

    const char *content = load_file(abs_path);
    if (!content) {
      atomic_fetch_sub(data->files_in_flight, 1);
      continue;
    }

    pthread_mutex_lock(data->global_mutex);
    printf("Formatting %s\n", abs_path);
    pthread_mutex_unlock(data->global_mutex);

    Arena thread_arena = {0};
    DiagList diags;
    diaglist_init(&diags, 1024);

    AstNode *ast = str_to_ast(&thread_arena, content, abs_path, &diags, false);
    if (!ast) {
      pthread_mutex_lock(data->global_mutex);
      for (size_t i = 0; i < diags.count; i++) {
        printf("Error on %u:%u in file %s: %s\n", diags.items[i].start_line,
               diags.items[i].start_char, diags.items[i].file,
               diags.items[i].message);
      }
      fprintf(stderr, "No AST found after trying to parse %s\n", abs_path);
      pthread_mutex_unlock(data->global_mutex);

      diaglist_free(&diags);
      free((void *)content);
      arena_free_all(&thread_arena);
      atomic_fetch_sub(data->files_in_flight, 1);
      continue;
    }
    diaglist_free(&diags);

    if (data->opts->print_ast) {
      pthread_mutex_lock(data->global_mutex);
      const char *mod_name = extract_mod_name(data->global_arena, abs_path);
      printf("Module: %s\n", mod_name);
      print_ast(ast);
      pthread_mutex_unlock(data->global_mutex);
    }

    if (data->opts->as.fmt.recursive) {
      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        if (stmt->type == AST_USE) {
          size_t path_len = stmt->as.use_stmt.path.len;
          if (path_len > 2) {
            pthread_mutex_lock(data->global_mutex);
            char *clean_rel = arena_alloc(data->global_arena, path_len - 1);
            strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
            clean_rel[path_len - 2] = '\0';
            // Fomatter should only check local deps to prevent libraries being
            // formatted
            const char *normalized =
                normalize_module_path(data->global_arena, clean_rel);
            pthread_mutex_unlock(data->global_mutex);

            atomic_fetch_add(data->files_in_flight, 1);
            wl_push(data->worklist, normalized);
          }
        }
        stmt = stmt->next;
      }
    }

    FILE *fp;
    if (data->opts->as.fmt.write) {
      fp = fopen(abs_path, "w+");
    } else {
      fp = stdout;
    }

    if (fp) {
      HashMap type_set;
      map_init(&type_set, &thread_arena, 128);
      collect_type_names(ast, &type_set, &thread_arena);

      // Prevent threads from stepping on each other if writing to stdout
      if (!data->opts->as.fmt.write) {
        pthread_mutex_lock(data->global_mutex);
      }

      fmt_ast(ast, fp, &type_set, content);

      if (!data->opts->as.fmt.write) {
        pthread_mutex_unlock(data->global_mutex);
      }

      if (data->opts->as.fmt.write) {
        fclose(fp);
      }
    }

    free((void *)content);
    arena_free_all(&thread_arena);
    atomic_fetch_sub(data->files_in_flight, 1);
  }
  return true;
}
#endif

bool fmt_project(const CompileOptions *restrict opts) {
  ensure_cache_dir();
  Arena arena = {0};

  HashMap seen;
  map_init(&seen, &arena, 1024);

  Worklist pending = {0};
  wl_init(&pending);
  wl_push(&pending, opts->input_file);

#ifdef ENABLE_THREADS
  int num_threads = opts->thread_count;
  if (num_threads <= 0) {
    num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads < 1)
      num_threads = 1;
    if (num_threads > 64)
      num_threads = 64;
  }

  ThreadPool *pool = tp_create(num_threads);
  pthread_mutex_t global_mutex;
  pthread_mutex_init(&global_mutex, NULL);
  atomic_size_t files_in_flight = 1;

  FmtWorkerData wdata = {.worklist = &pending,
                         .global_arena = &arena,
                         .global_mutex = &global_mutex,
                         .seen = &seen,
                         .files_in_flight = &files_in_flight,
                         .opts = opts};

  for (int i = 0; i < num_threads; i++) {
    tp_submit(pool, (TaskFunc)fmt_worker_loop, &wdata);
  }

  while (atomic_load(&files_in_flight) > 0) {
    sched_yield();
  }
  wl_done(&pending);

  tp_wait(pool);
  tp_destroy(pool);
  pthread_mutex_destroy(&global_mutex);

#else
  int files_in_flight = 1;
  const char *current_path;

  while (files_in_flight > 0 && (current_path = wl_pop(&pending)) != NULL) {
    const char *abs_path = resolve_alloc(&arena, current_path);
    if (!abs_path ||
        map_get(&seen, current_path, strlen(current_path)) != NULL) {
      files_in_flight--;
      if (files_in_flight == 0)
        wl_done(&pending);
      continue;
    }

    map_set(&seen, current_path, strlen(current_path), (void *)1);

    const char *content = load_file(abs_path);
    if (!content) {
      files_in_flight--;
      if (files_in_flight == 0)
        wl_done(&pending);
      continue;
    }

    printf("Formatting %s\n", abs_path);

    DiagList diags;
    diaglist_init(&diags, 1024);

    AstNode *ast = str_to_ast(&arena, content, abs_path, &diags, false);
    if (!ast) {
      for (size_t i = 0; i < diags.count; i++) {
        printf("Error on %u:%u in file %s: %s\n", diags.items[i].start_line,
               diags.items[i].start_char, diags.items[i].file,
               diags.items[i].message);
      }
      fprintf(stderr, "No AST found after trying to parse %s\n", abs_path);
      diaglist_free(&diags);
      free((void *)content);
      continue;
    }
    diaglist_free(&diags);

    if (opts->print_ast) {
      const char *mod_name = extract_mod_name(&arena, abs_path);
      printf("Module: %s\n", mod_name);
      print_ast(ast);
    }

    if (opts->as.fmt.recursive) {
      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        if (stmt->type == AST_USE) {
          size_t path_len = stmt->as.use_stmt.path.len;
          if (path_len > 2) {
            char *clean_rel = arena_alloc(&arena, path_len - 1);
            strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
            clean_rel[path_len - 2] = '\0';
            // Fomatter should only check local deps to prevent libraries being
            // formatted
            const char *normalized = normalize_module_path(&arena, clean_rel);

            files_in_flight++;
            wl_push(&pending, normalized);
          }
        }
        stmt = stmt->next;
      }
    }

    FILE *fp;
    if (opts->as.fmt.write) {
      fp = fopen(abs_path, "w+");
    } else {
      fp = stdout;
    }

    if (fp) {
      HashMap type_set;
      map_init(&type_set, &arena, 128);
      collect_type_names(ast, &type_set, &arena);

      fmt_ast(ast, fp, &type_set, content);
      if (opts->as.fmt.write)
        fclose(fp);
    }

    free(content);

    files_in_flight--;
    if (files_in_flight == 0) {
      wl_done(&pending);
    }
  }
#endif

  if (pending.paths)
    free(pending.paths);
  arena_free_all(&arena);
  return true;
}
