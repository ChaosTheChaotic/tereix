#include "fmt.h"
#include "arena.h"
#include "ast_types.h"
#include "diag.h"
#include "hashmap.h"
#include "util.h"
#include "worklist.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void collect_type_names(AstNode *root, HashMap *type_set, Arena *arena) {
  if (!root)
    return;

  size_t cap = 1024;
  AstNode **stack = malloc(sizeof(AstNode *) * cap);
  size_t top = 0;
  stack[top++] = root;

  while (top > 0) {
    AstNode *node = stack[--top];
    if (!node)
      continue;

    // Record type names
    if (node->type == AST_STRUCT) {
      Token name = node->as.struct_def.structn;
      if (name.len > 0) {
        char *type_name = arena_alloc(arena, name.len + 1);
        memcpy(type_name, name.start, name.len);
        type_name[name.len] = '\0';
        map_set(type_set, type_name, name.len, (void *)(uintptr_t)1);
      }
    } else if (node->type == AST_UNION) {
      Token name = node->as.union_def.unionn;
      if (name.len > 0) {
        char *type_name = arena_alloc(arena, name.len + 1);
        memcpy(type_name, name.start, name.len);
        type_name[name.len] = '\0';
        map_set(type_set, type_name, name.len, (void *)(uintptr_t)1);
      }
    } else if (node->type == AST_ENUM) {
      Token name = node->as.enum_def.enumn;
      if (name.len > 0) {
        char *type_name = arena_alloc(arena, name.len + 1);
        memcpy(type_name, name.start, name.len);
        type_name[name.len] = '\0';
        map_set(type_set, type_name, name.len, (void *)(uintptr_t)1);
      }
    }

    // Push children and siblings
    if (node->next) {
      if (top >= cap) {
        cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * cap);
      }
      stack[top++] = node->next;
    }

#define PUSH_CHILD(n)                                                          \
  do {                                                                         \
    if (n) {                                                                   \
      if (top >= cap) {                                                        \
        cap *= 2;                                                              \
        stack = realloc(stack, sizeof(AstNode *) * cap);                       \
      }                                                                        \
      stack[top++] = (n);                                                      \
    }                                                                          \
  } while (0)

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
      PUSH_CHILD(node->as.block.first_stmt);
      break;
    case AST_FUNC:
      PUSH_CHILD(node->as.func_def.params);
      PUSH_CHILD(node->as.func_def.block);
      break;
    case AST_VAR_DECL:
      PUSH_CHILD(node->as.var_decl.init);
      break;
    case AST_BINOP:
      PUSH_CHILD(node->as.binop.left);
      PUSH_CHILD(node->as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PUSH_CHILD(node->as.unop.operand);
      break;
    case AST_IF:
      PUSH_CHILD(node->as.if_check.check);
      PUSH_CHILD(node->as.if_check.action);
      PUSH_CHILD(node->as.if_check.elseAct);
      break;
    case AST_WHILE:
      PUSH_CHILD(node->as.while_loop.check);
      PUSH_CHILD(node->as.while_loop.action);
      break;
    case AST_FOR:
      PUSH_CHILD(node->as.for_loop.init);
      PUSH_CHILD(node->as.for_loop.check);
      PUSH_CHILD(node->as.for_loop.inc);
      PUSH_CHILD(node->as.for_loop.action);
      break;
    case AST_FUNC_CALL:
      PUSH_CHILD(node->as.func_call.caller);
      PUSH_CHILD(node->as.func_call.args);
      break;
    case AST_INDEX:
      PUSH_CHILD(node->as.index.base);
      PUSH_CHILD(node->as.index.index);
      break;
    case AST_MEMBER:
      PUSH_CHILD(node->as.member.base);
      break;
    case AST_ARRAY_LIT:
      PUSH_CHILD(node->as.array_lit.elements);
      break;
    case AST_STRUCT:
    case AST_UNION:
    case AST_ENUM:
      PUSH_CHILD((node->type == AST_STRUCT)  ? node->as.struct_def.contents
                 : (node->type == AST_UNION) ? node->as.union_def.contents
                                             : node->as.enum_def.contents);
      break;
    case AST_CAST:
      PUSH_CHILD(node->as.cast.op);
      break;
    case AST_RET:
      PUSH_CHILD(node->as.ret_stmt.expr);
      break;
    case AST_SWITCH:
      PUSH_CHILD(node->as.switch_stmt.check);
      PUSH_CHILD(node->as.switch_stmt.cases);
      PUSH_CHILD(node->as.switch_stmt.default_case);
      break;
    case AST_CASE:
      PUSH_CHILD(node->as.case_stmt.val);
      PUSH_CHILD(node->as.case_stmt.action);
      break;
    case AST_DEFER:
      PUSH_CHILD(node->as.defer_stmt.contents);
      break;
    case AST_EXTERN:
      PUSH_CHILD(node->as.extern_block.contents);
      break;
    case AST_SIZEOF:
      if (!node->as.sizeof_expr.is_type && node->as.sizeof_expr.target_expr)
        PUSH_CHILD(node->as.sizeof_expr.target_expr);
      break;
    default:
      break;
    }
#undef PUSH_CHILD
  }
  free(stack);
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

bool fmt_ast(AstNode *root, FILE *out_fp, HashMap *type_set) {
  if (!root) {
    fprintf(stderr, "No root AST was passed to fmt_ast");
    return false;
  }

  Arena tmp_arena = {0};
  HashMap extern_set;
  map_init(&extern_set, &tmp_arena, 128);

  size_t ext_cap = 1024;
  AstNode **ext_stack = malloc(sizeof(AstNode *) * ext_cap);
  size_t ext_top = 0;
  ext_stack[ext_top++] = root;

  while (ext_top > 0) {
    AstNode *n = ext_stack[--ext_top];
    if (!n)
      continue;

    if (n->type == AST_EXTERN) {
      AstNode *curr = n->as.extern_block.contents;
      while (curr) {
        if (curr->type == AST_FUNC) {
          map_set(&extern_set, curr->as.func_def.fn_name.start,
                  curr->as.func_def.fn_name.len, (void *)(uintptr_t)1);
        } else if (curr->type == AST_VAR_DECL) {
          map_set(&extern_set, curr->as.var_decl.id.start,
                  curr->as.var_decl.id.len, (void *)(uintptr_t)1);
        } else if (curr->type == AST_STRUCT) {
          map_set(&extern_set, curr->as.struct_def.structn.start,
                  curr->as.struct_def.structn.len, (void *)(uintptr_t)1);
        } else if (curr->type == AST_UNION) {
          map_set(&extern_set, curr->as.union_def.unionn.start,
                  curr->as.union_def.unionn.len, (void *)(uintptr_t)1);
        }
        curr = curr->next;
      }
    } else if (n->type == AST_FUNC && n->as.func_def.is_extern) {
      map_set(&extern_set, n->as.func_def.fn_name.start,
              n->as.func_def.fn_name.len, (void *)(uintptr_t)1);
    } else if (n->type == AST_VAR_DECL && n->as.var_decl.type.is_extern) {
      map_set(&extern_set, n->as.var_decl.id.start, n->as.var_decl.id.len,
              (void *)(uintptr_t)1);
    } else if (n->type == AST_STRUCT && n->as.struct_def.is_extern) {
      map_set(&extern_set, n->as.struct_def.structn.start,
              n->as.struct_def.structn.len, (void *)(uintptr_t)1);
    } else if (n->type == AST_UNION && n->as.union_def.is_extern) {
      map_set(&extern_set, n->as.union_def.unionn.start,
              n->as.union_def.unionn.len, (void *)(uintptr_t)1);
    }

    if (n->next) {
      if (ext_top >= ext_cap) {
        ext_cap *= 2;
        ext_stack = realloc(ext_stack, sizeof(AstNode *) * ext_cap);
      }
      ext_stack[ext_top++] = n->next;
    }

    if (n->type == AST_PROGRAM || n->type == AST_BLOCK) {
      AstNode *child = n->as.block.first_stmt;
      if (child) {
        if (ext_top >= ext_cap) {
          ext_cap *= 2;
          ext_stack = realloc(ext_stack, sizeof(AstNode *) * ext_cap);
        }
        ext_stack[ext_top++] = child;
      }
    }
  }
  free(ext_stack);

#define FPRINTF_SAFE(format, ...)                                              \
  do {                                                                         \
    if (fprintf(out_fp, format, __VA_ARGS__) < 0) {                            \
      fprintf(stderr, "Failed to fully write to file\n");                      \
      if (ferror(out_fp)) {                                                    \
        fprintf(stderr, "Error details: %s\n", strerror(errno));               \
      }                                                                        \
      goto err_cleanup;                                                        \
    }                                                                          \
  } while (0)

#define FMT_TYPE_SAFE(t)                                                       \
  do {                                                                         \
    if (t.is_static)                                                           \
      FPRINTF_SAFE("%s", "static ");                                           \
    if (t.is_mut)                                                              \
      FPRINTF_SAFE("%s", "mut ");                                              \
    if (t.is_threadlocal)                                                      \
      FPRINTF_SAFE("%s", "threadlocal ");                                      \
    if (t.is_extern)                                                           \
      FPRINTF_SAFE("%s", "extern ");                                           \
    if (t.is_async)                                                            \
      FPRINTF_SAFE("%s", "async ");                                            \
    if (t.is_inline)                                                           \
      FPRINTF_SAFE("%s", "inline ");                                           \
    if (t.ptr_depth != 0) {                                                    \
      char sym = (t.ptr_depth > 0) ? '*' : '&';                                \
      int cnt = (t.ptr_depth > 0) ? t.ptr_depth : -t.ptr_depth;                \
      for (int _i = 0; _i < cnt; _i++)                                         \
        FPRINTF_SAFE("%c", sym);                                               \
    }                                                                          \
    if (t.name.len > 0) {                                                      \
      if (is_builtin_type_name(t.name.start, t.name.len) ||                    \
          map_get(&extern_set, t.name.start, t.name.len) != NULL) {            \
        FPRINTF_SAFE("%.*s", (int)t.name.len, t.name.start);                   \
      } else {                                                                 \
        char *f_type =                                                         \
            format_identifier(t.name.start, t.name.len, FMT_PASCAL_CASE);      \
        FPRINTF_SAFE("%s", f_type);                                            \
        free(f_type);                                                          \
      }                                                                        \
    }                                                                          \
    for (unsigned int _i = 0; _i < t.array_dimens; _i++) {                     \
      if (t.dim_sizes && _i < t.array_dimens && t.dim_sizes[_i]) {             \
        AstNode *dim = t.dim_sizes[_i];                                        \
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

  size_t stack_cap = 1024;
  FmtStackItem *stack = malloc(sizeof(FmtStackItem) * stack_cap);
  size_t top = 0;

  stack[top++] = (FmtStackItem){root, 0, 0, NULL};

  while (top > 0) {
    FmtStackItem *item = &stack[top - 1];
    AstNode *node = item->node;

    if (!node) {
      top--;
      continue;
    }

    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
    case AST_EXTERN: {
      if (item->step == 0) {
        if (node->type == AST_BLOCK) {
          FPRINTF_SAFE("%s", "{\n");
        } else if (node->type == AST_EXTERN) {
          FPRINTF_SAFE("%s", "extern {\n");
        }
        item->aux = (node->type == AST_EXTERN) ? node->as.extern_block.contents
                                               : node->as.block.first_stmt;
        item->step = 1;
      } else if (item->step == 1) {
        if (item->aux) {
          AstNode *stmt = item->aux;
          item->aux = stmt->next;

          unsigned int next_depth =
              item->depth + (node->type == AST_PROGRAM ? 0 : 1);
          for (unsigned int i = 0; i < next_depth; i++)
            FPRINTF_SAFE("%c", '\t');

          // Check if this statement needs a terminating semicolon applied by
          // the block
          bool needs_semi =
              (stmt->type == AST_FUNC_CALL || stmt->type == AST_BINOP ||
               stmt->type == AST_UOP || stmt->type == AST_IDENTIF ||
               stmt->type == AST_NUM_LIT || stmt->type == AST_STR_LIT ||
               stmt->type == AST_CHAR_LIT || stmt->type == AST_BOOL_LIT ||
               stmt->type == AST_NULL_LIT || stmt->type == AST_MEMBER ||
               stmt->type == AST_ARRAY_LIT || stmt->type == AST_INDEX ||
               stmt->type == AST_CAST || stmt->type == AST_SIZEOF);

          bool is_block = (stmt->type == AST_BLOCK);

          if (needs_semi)
            item->step = 2;
          else if (is_block)
            item->step = 3;
          else
            item->step = 5;

          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){stmt, next_depth, 0, NULL};
        } else {
          item->step = 4;
        }
      } else if (item->step == 2 || item->step == 3 || item->step == 5) {
        if (item->step == 2)
          FPRINTF_SAFE("%s", ";\n");
        else if (item->step == 3)
          FPRINTF_SAFE("%s", "\n");
        item->step = 1;
      } else if (item->step == 4) {
        if (node->type == AST_BLOCK || node->type == AST_EXTERN) {
          for (unsigned int i = 0; i < item->depth; i++)
            FPRINTF_SAFE("%c", '\t');
          FPRINTF_SAFE("%s", "}");
          // Blocks do not print \n so IF/WHILE/FOR can control trailing flow
          if (node->type == AST_EXTERN)
            FPRINTF_SAFE("%s", "\n\n");
        }
        top--;
      }
      break;
    }

    case AST_FUNC: {
      if (item->step == 0) {
        FMT_TYPE_SAFE(node->as.func_def.ret_type);
        FPRINTF_SAFE("%s", " ");

        bool is_ext = map_get(&extern_set, node->as.func_def.fn_name.start,
                              node->as.func_def.fn_name.len) != NULL;
        if (is_ext) {
          FPRINTF_SAFE("%.*s(", (int)node->as.func_def.fn_name.len,
                       node->as.func_def.fn_name.start);
        } else {
          char *f_name =
              format_identifier(node->as.func_def.fn_name.start,
                                node->as.func_def.fn_name.len, FMT_SNAKE_CASE);
          FPRINTF_SAFE("%s(", f_name);
          free(f_name);
        }

        AstNode *param = node->as.func_def.params;
        while (param) {
          FMT_TYPE_SAFE(param->as.fn_param.type);
          FPRINTF_SAFE("%s", " ");
          char *p_name =
              format_identifier(param->as.fn_param.id.start,
                                param->as.fn_param.id.len, FMT_CAMEL_CASE);
          FPRINTF_SAFE("%s", p_name);
          free(p_name);
          if (param->next)
            FPRINTF_SAFE("%s", ", ");
          param = param->next;
        }
        FPRINTF_SAFE("%s", ")");

        if (node->as.func_def.block) {
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.func_def.block, item->depth, 0, NULL};
          FPRINTF_SAFE("%s", " ");
        } else {
          FPRINTF_SAFE("%s", ";\n");
          top--;
        }
      } else {
        FPRINTF_SAFE("%s", "\n"); // block ended without \n
        if (node->next)
          FPRINTF_SAFE("%s", "\n");

        top--;
      }
      break;
    }

    case AST_VAR_DECL: {
      if (item->step == 0) {
        FMT_TYPE_SAFE(node->as.var_decl.type);
        FPRINTF_SAFE("%s", " ");

        bool is_ext = map_get(&extern_set, node->as.var_decl.id.start,
                              node->as.var_decl.id.len) != NULL;
        if (is_ext) {
          FPRINTF_SAFE("%.*s", (int)node->as.var_decl.id.len,
                       node->as.var_decl.id.start);
        } else {
          char *v_name =
              format_identifier(node->as.var_decl.id.start,
                                node->as.var_decl.id.len, FMT_CAMEL_CASE);
          FPRINTF_SAFE("%s", v_name);
          free(v_name);
        }

        if (node->as.var_decl.init) {
          FPRINTF_SAFE("%s", " = ");
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.var_decl.init, item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", item->aux ? "; " : ";\n");
          top--;
        }
      } else {
        FPRINTF_SAFE("%s", item->aux ? "; " : ";\n");
        top--;
      }
      break;
    }

    case AST_STRUCT:
    case AST_UNION:
    case AST_ENUM: {
      if (item->step == 0) {
        const char *kw = (node->type == AST_STRUCT)  ? "struct"
                         : (node->type == AST_UNION) ? "union"
                                                     : "enum";
        Token name_tok = (node->type == AST_STRUCT)
                             ? node->as.struct_def.structn
                         : (node->type == AST_UNION) ? node->as.union_def.unionn
                                                     : node->as.enum_def.enumn;

        bool is_decl_only = false;
        if (node->src_end && node->src_end > node->src_start) {
          if (*(node->src_end - 1) == ';') {
            is_decl_only = true;
          }
        }

        bool is_extern_node = false;
        if (node->type == AST_STRUCT && node->as.struct_def.is_extern)
          is_extern_node = true;
        if (node->type == AST_UNION && node->as.union_def.is_extern)
          is_extern_node = true;

        bool is_ext =
            map_get(&extern_set, name_tok.start, name_tok.len) != NULL ||
            is_extern_node;

        if (is_extern_node) {
          FPRINTF_SAFE("%s", "extern ");
        }

        FPRINTF_SAFE("%s ", kw);
        if (is_decl_only) {
          if (is_ext) {
            FPRINTF_SAFE("%.*s;\n\n", (int)name_tok.len, name_tok.start);
          } else {
            char *t_name = format_identifier(name_tok.start, name_tok.len,
                                             FMT_PASCAL_CASE);
            FPRINTF_SAFE("%s;\n", t_name);
            free(t_name);
          }
          top--;
          break;
        }

        if (is_ext) {
          FPRINTF_SAFE("%.*s {\n", (int)name_tok.len, name_tok.start);
        } else {
          char *t_name =
              format_identifier(name_tok.start, name_tok.len, FMT_PASCAL_CASE);
          FPRINTF_SAFE("%s {\n", t_name);
          free(t_name);
        }

        item->aux = (node->type == AST_STRUCT)  ? node->as.struct_def.contents
                    : (node->type == AST_UNION) ? node->as.union_def.contents
                                                : node->as.enum_def.contents;
        item->step = 1;
      } else if (item->step == 1) {
        if (item->aux) {
          AstNode *mem = item->aux;
          item->aux = mem->next;
          for (unsigned int i = 0; i < item->depth + 1; i++)
            FPRINTF_SAFE("%c", '\t');

          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){mem, item->depth + 1, 0, NULL};
        } else {
          item->step = 2;
        }
      } else {
        for (unsigned int i = 0; i < item->depth; i++)
          FPRINTF_SAFE("%c", '\t');
        FPRINTF_SAFE("%s", "}\n\n");
        top--;
      }
      break;
    }

    case AST_FUNC_CALL: {
      if (item->step == 0) {
        if (node->as.func_call.caller->type == AST_IDENTIF) {
          Token ident = node->as.func_call.caller->as.identif.val;

          if (map_get(&extern_set, ident.start, ident.len) != NULL) {
            FPRINTF_SAFE("%.*s(", (int)ident.len, ident.start);
          } else {
            char *fn_name =
                format_identifier(ident.start, ident.len, FMT_SNAKE_CASE);
            FPRINTF_SAFE("%s(", fn_name);
            free(fn_name);
          }
          item->step = 2;
        } else {
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.func_call.caller, item->depth, 0, NULL};
        }
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", "(");
        item->step = 2;
      } else if (item->step == 2) {
        item->aux = node->as.func_call.args;
        item->step = 3;
      } else if (item->step == 3) {
        if (item->aux) {
          AstNode *arg = item->aux;
          item->aux = arg->next;
          item->step = item->aux ? 4 : 5;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){arg, item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", ")");
          top--;
        }
      } else if (item->step == 4) {
        FPRINTF_SAFE("%s", ", ");
        item->step = 3;
      } else if (item->step == 5) {
        FPRINTF_SAFE("%s", ")");
        top--;
      }
      break;
    }

    case AST_IDENTIF: {
      Token ident = node->as.identif.val;

      if (map_get(&extern_set, ident.start, ident.len) != NULL) {
        FPRINTF_SAFE("%.*s", (int)ident.len, ident.start);
        top--;
        break;
      }

      char *formatted = NULL;
      if (type_set && map_get(type_set, ident.start, ident.len)) {
        formatted = format_identifier(ident.start, ident.len, FMT_PASCAL_CASE);
      } else {
        formatted = format_identifier(ident.start, ident.len, FMT_CAMEL_CASE);
      }
      FPRINTF_SAFE("%s", formatted);
      free(formatted);
      top--;
      break;
    }

    case AST_BINOP: {
      if (item->step == 0) {
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.binop.left, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE(" %.*s ", node->as.binop.op.len, node->as.binop.op.start);
        item->step = 2;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.binop.right, item->depth, 0, NULL};
      } else {
        top--;
      }
      break;
    }

    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF: {
      if (item->step == 0) {
        if (!node->as.unop.is_postfix) {
          FPRINTF_SAFE("%.*s", node->as.unop.op.len, node->as.unop.op.start);
        }
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.unop.operand, item->depth, 0, NULL};
      } else {
        if (node->as.unop.is_postfix) {
          FPRINTF_SAFE("%.*s", node->as.unop.op.len, node->as.unop.op.start);
        }
        top--;
      }
      break;
    }

    case AST_IF: {
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "if (");
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.if_check.check, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", ") ");
        item->step = 2;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.if_check.action, item->depth, 0, NULL};
      } else if (item->step == 2) {
        if (node->as.if_check.action->type != AST_BLOCK)
          FPRINTF_SAFE("%s", ";\n");
        if (node->as.if_check.elseAct) {
          if (node->as.if_check.action->type == AST_BLOCK) {
            FPRINTF_SAFE("%s", " ");
          } else {
            for (unsigned int i = 0; i < item->depth; i++)
              FPRINTF_SAFE("%c", '\t');
          }
          FPRINTF_SAFE("%s", "else ");
          item->step = 3;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.if_check.elseAct, item->depth, 0, NULL};
        } else {
          if (node->as.if_check.action->type == AST_BLOCK)
            FPRINTF_SAFE("%s", "\n");
          top--;
        }
      } else {
        if (node->as.if_check.elseAct->type == AST_BLOCK)
          FPRINTF_SAFE("%s", "\n");
        else
          FPRINTF_SAFE("%s", ";\n");
        top--;
      }
      break;
    }

    case AST_NUM_LIT:
      FPRINTF_SAFE("%.*s", node->as.num_lit.val.len,
                   node->as.num_lit.val.start);
      top--;
      break;

    case AST_STR_LIT:
      FPRINTF_SAFE("%.*s", node->as.str_lit.val.len,
                   node->as.str_lit.val.start);
      top--;
      break;

    case AST_CHAR_LIT:
      FPRINTF_SAFE("%.*s", node->as.char_lit.val.len,
                   node->as.char_lit.val.start);
      top--;
      break;

    case AST_BOOL_LIT:
      FPRINTF_SAFE("%.*s", node->as.bool_lit.val.len,
                   node->as.bool_lit.val.start);
      top--;
      break;

    case AST_NULL_LIT:
      FPRINTF_SAFE("%s", "null");
      top--;
      break;

    case AST_RET: {
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "ret");
        if (node->as.ret_stmt.expr) {
          FPRINTF_SAFE("%s", " ");
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.ret_stmt.expr, item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", ";\n");
          top--;
        }
      } else {
        FPRINTF_SAFE("%s", ";\n");
        top--;
      }
      break;
    }

    case AST_ARRAY_LIT:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "[");
        item->aux = node->as.array_lit.elements;
        item->step = 1;
      } else if (item->step == 1) {
        if (item->aux) {
          AstNode *elem = item->aux;
          item->aux = item->aux->next;
          item->step = (item->aux != NULL) ? 2 : 3;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){elem, item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", "]");
          top--;
        }
      } else if (item->step == 2) {
        FPRINTF_SAFE("%s", ", ");
        item->step = 1;
      } else {
        FPRINTF_SAFE("%s", "]");
        top--;
      }
      break;

    case AST_ENUM_MEMBER:
      if (item->step == 0) {
        char *m_name =
            format_identifier(node->as.enum_member.name.start,
                              node->as.enum_member.name.len, FMT_PASCAL_CASE);
        FPRINTF_SAFE("%s", m_name);
        free(m_name);
        if (node->as.enum_member.val) {
          FPRINTF_SAFE("%s", " = ");
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.enum_member.val, item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", ",\n");
          top--;
        }
      } else {
        FPRINTF_SAFE("%s", ",\n");
        top--;
      }
      break;

    case AST_DEFER:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "defer ");
        item->step = 1;
        if (node->as.defer_stmt.contents) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){node->as.defer_stmt.contents,
                                        item->depth, 0, NULL};
        } else {
          FPRINTF_SAFE("%s", ";\n");
          top--;
        }
      } else {
        if (node->as.defer_stmt.contents &&
            node->as.defer_stmt.contents->type == AST_BLOCK) {
          FPRINTF_SAFE("%s", "\n");
        } else {
          FPRINTF_SAFE("%s", ";\n");
        }
        top--;
      }
      break;

    case AST_FOR:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "for (");
        item->step = 1;
        if (node->as.for_loop.init) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.for_loop.init, item->depth, 0, (void *)1};
        }
      } else if (item->step == 1) {
        if (!node->as.for_loop.init ||
            node->as.for_loop.init->type != AST_VAR_DECL)
          FPRINTF_SAFE("%s", "; ");
        item->step = 2;
        if (node->as.for_loop.check) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.for_loop.check, item->depth, 0, NULL};
        }
      } else if (item->step == 2) {
        FPRINTF_SAFE("%s", "; ");
        item->step = 3;
        if (node->as.for_loop.inc) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.for_loop.inc, item->depth, 0, NULL};
        }
      } else if (item->step == 3) {
        FPRINTF_SAFE("%s", ") ");
        item->step = 4;
        if (node->as.for_loop.action) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.for_loop.action, item->depth, 0, NULL};
        }
      } else {
        if (node->as.for_loop.action->type == AST_BLOCK)
          FPRINTF_SAFE("%s", "\n");
        else
          FPRINTF_SAFE("%s", ";\n");
        top--;
      }
      break;

    case AST_WHILE:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "while (");
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.while_loop.check, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", ") ");
        item->step = 2;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.while_loop.action, item->depth, 0, NULL};
      } else {
        if (node->as.while_loop.action->type == AST_BLOCK)
          FPRINTF_SAFE("%s", "\n");
        else
          FPRINTF_SAFE("%s", ";\n");
        top--;
      }
      break;

    case AST_INDEX:
      if (item->step == 0) {
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.index.base, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", "[");
        item->step = 2;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.index.index, item->depth, 0, NULL};
      } else {
        FPRINTF_SAFE("%s", "]");
        top--;
      }
      break;

    case AST_MEMBER:
      if (item->step == 0) {
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.member.base, item->depth, 0, NULL};
      } else {
        FPRINTF_SAFE("%s", ".");

        if (map_get(&extern_set, node->as.member.name.start,
                    node->as.member.name.len) != NULL) {
          FPRINTF_SAFE("%.*s", (int)node->as.member.name.len,
                       node->as.member.name.start);
        } else {
          char *m_name =
              format_identifier(node->as.member.name.start,
                                node->as.member.name.len, FMT_CAMEL_CASE);
          FPRINTF_SAFE("%s", m_name);
          free(m_name);
        }
        top--;
      }
      break;

    case AST_SWITCH:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "switch (");
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.switch_stmt.check, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", ") {\n");
        item->aux = node->as.switch_stmt.cases;
        item->step = 2;
      } else if (item->step == 2) {
        if (item->aux) {
          AstNode *c = item->aux;
          item->aux = c->next;
          for (unsigned int i = 0; i < item->depth + 1; i++)
            FPRINTF_SAFE("%c", '\t');
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){c, item->depth + 1, 0, NULL};
        } else if (node->as.switch_stmt.default_case) {
          for (unsigned int i = 0; i < item->depth + 1; i++)
            FPRINTF_SAFE("%c", '\t');
          FPRINTF_SAFE("%s", "default ");
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){node->as.switch_stmt.default_case,
                                        item->depth + 1, 0, NULL};
          item->step = 3;
        } else {
          for (unsigned int i = 0; i < item->depth; i++)
            FPRINTF_SAFE("%c", '\t');
          FPRINTF_SAFE("%s", "}\n");
          top--;
        }
      } else if (item->step == 3) {
        FPRINTF_SAFE("%s", "\n");
        for (unsigned int i = 0; i < item->depth; i++)
          FPRINTF_SAFE("%c", '\t');
        FPRINTF_SAFE("%s", "}\n");
        top--;
      }
      break;

    case AST_CASE:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "case (");
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] =
            (FmtStackItem){node->as.case_stmt.val, item->depth, 0, NULL};
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", ") ");
        item->step = 2;
        if (node->as.case_stmt.action) {
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] =
              (FmtStackItem){node->as.case_stmt.action, item->depth, 0, NULL};
        }
      } else if (item->step == 2) {
        if (node->as.case_stmt.action &&
            node->as.case_stmt.action->type == AST_BLOCK)
          FPRINTF_SAFE("%s", "\n");
        else
          FPRINTF_SAFE("%s", ";\n");
        top--;
      }
      break;

    case AST_USE:
      FPRINTF_SAFE("%s", "use ");
      FPRINTF_SAFE("%.*s", (int)node->as.use_stmt.path.len,
                   node->as.use_stmt.path.start);
      if (node->as.use_stmt.alias.len > 0) {
        FPRINTF_SAFE("%s", " as ");
        char *alias_name =
            format_identifier(node->as.use_stmt.alias.start,
                              node->as.use_stmt.alias.len, FMT_CAMEL_CASE);
        FPRINTF_SAFE("%s", alias_name);
        free(alias_name);
      }
      FPRINTF_SAFE("%s", ";\n");

      if (node->next && node->next->type != AST_USE) {
        FPRINTF_SAFE("%s", "\n");
      }

      top--;
      break;

    case AST_BREAK:
      FPRINTF_SAFE("%s", "break;\n");
      top--;
      break;

    case AST_CONTINUE:
      FPRINTF_SAFE("%s", "continue;\n");
      top--;
      break;

    case AST_CAST:
      if (item->step == 0) {
        FPRINTF_SAFE("%s", "(");
        if (is_builtin_type_name(node->as.cast.target.name.start,
                                 node->as.cast.target.name.len)) {
          FPRINTF_SAFE("%.*s", (int)node->as.cast.target.name.len,
                       node->as.cast.target.name.start);
        } else {
          char *type_name =
              format_identifier(node->as.cast.target.name.start,
                                node->as.cast.target.name.len, FMT_PASCAL_CASE);
          FPRINTF_SAFE("%s", type_name);
          free(type_name);
        }
        FPRINTF_SAFE("%s", ")");
        item->step = 1;
        if (top >= stack_cap) {
          stack_cap *= 2;
          stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
          item = &stack[top - 1];
        }
        stack[top++] = (FmtStackItem){node->as.cast.op, item->depth, 0, NULL};
      } else {
        top--;
      }
      break;

    case AST_SIZEOF:
      if (item->step == 0) {
        if (!node->as.sizeof_expr.is_type && node->as.sizeof_expr.target_expr &&
            node->as.sizeof_expr.target_expr->type == AST_CAST) {
          AstNode *cast = node->as.sizeof_expr.target_expr;
          AstNode *op = cast->as.cast.op;

          if (op && op->type == AST_IDENTIF &&
              map_get(type_set, op->as.identif.val.start,
                      op->as.identif.val.len) != NULL) {
            FPRINTF_SAFE("%s", "(");
            FMT_TYPE_SAFE(cast->as.cast.target);
            FPRINTF_SAFE("%s", ")");
            FPRINTF_SAFE("%s", "sizeof(");
            FPRINTF_SAFE("%.*s", (int)op->as.identif.val.len,
                         op->as.identif.val.start);
            FPRINTF_SAFE("%s", ")");
            top--;
            break;
          }
        }
        FPRINTF_SAFE("%s", "sizeof(");
        if (node->as.sizeof_expr.is_type) {
          if (is_builtin_type_name(node->as.sizeof_expr.target_type.name.start,
                                   node->as.sizeof_expr.target_type.name.len)) {
            FPRINTF_SAFE("%.*s", (int)node->as.sizeof_expr.target_type.name.len,
                         node->as.sizeof_expr.target_type.name.start);
          } else {
            char *type_name = format_identifier(
                node->as.sizeof_expr.target_type.name.start,
                node->as.sizeof_expr.target_type.name.len, FMT_PASCAL_CASE);
            FPRINTF_SAFE("%s", type_name);
            free(type_name);
          }
          FPRINTF_SAFE("%s", ")");
          top--;
        } else {
          item->step = 1;
          if (top >= stack_cap) {
            stack_cap *= 2;
            stack = realloc(stack, sizeof(FmtStackItem) * stack_cap);
            item = &stack[top - 1];
          }
          stack[top++] = (FmtStackItem){node->as.sizeof_expr.target_expr,
                                        item->depth, 0, NULL};
        }
      } else if (item->step == 1) {
        FPRINTF_SAFE("%s", ")");
        top--;
      }
      break;
    case AST_PARAM:
      break;
    }
  }

  free(stack);
  return true;

err_cleanup:
  fclose(out_fp);
  free(stack);
  return false;
}

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
						// Fomatter should only check local deps to prevent libraries being formatted
            const char *normalized = normalize_module_path(data->global_arena, clean_rel);
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

      fmt_ast(ast, fp, &type_set);

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
      exit(1);
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
						// Fomatter should only check local deps to prevent libraries being formatted
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

      fmt_ast(ast, fp, &type_set);
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
