#include "lsp.h"
#include "arena.h"
#include "ast_types.h"
#include "diag.h"
#include "sem_types.h"
#include "util.h"
#include "worklist.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Module *sem_current_mod;
extern Module *sem_main_mod;

static LspState server_state = {.state = UNINITIALIZED};

Doc *get_or_load_doc(const char *uri, const char *abs_path) {
  Doc *d = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (d)
    return d;

  const char *file_txt = load_file(abs_path);
  if (!file_txt)
    return NULL;

  d = malloc(sizeof(Doc));
  d->uri = strdup(uri);
  d->txt = strdup(file_txt);
  free((void *)file_txt);
  d->version = 0;

  d->ast_arena = malloc(sizeof(Arena));
  *d->ast_arena = (Arena){0};

  DiagList diags;
  diaglist_init(&diags, 1024);
  d->ast_root = str_to_ast(d->ast_arena, d->txt, abs_path, &diags, true);
  diaglist_free(&diags);

  map_set(&server_state.docs, d->uri, strlen(d->uri), d);
  server_state.doc_count++;
  return d;
}

void extract_use_namespace(AstNode *use_stmt, char *out_name, size_t max_len) {
  Token alias = use_stmt->as.use_stmt.alias;
  if (alias.len > 0) {
    snprintf(out_name, max_len, "%.*s", (int)alias.len, alias.start);
    return;
  }

  Token pt = use_stmt->as.use_stmt.path;
  if (pt.len >= 2) {
    char tmp[512] = {0};
    int copy_len = pt.len - 2 < 511 ? pt.len - 2 : 511;
    strncpy(tmp, pt.start + 1, copy_len);

    char *base = strrchr(tmp, '/');
    base = base ? base + 1 : tmp;
    char *dot = strrchr(base, '.');
    if (dot)
      *dot = '\0';

    snprintf(out_name, max_len, "%s", base);
  }
}

size_t token_to_buf(Token t, char *buf, size_t buf_size) {
  if (!t.start || t.len == 0 || buf_size == 0) {
    if (buf_size > 0)
      buf[0] = '\0';
    return 0;
  }
  // Find first null byte within the token length
  size_t actual_len = strnlen(t.start, t.len);
  if (actual_len == 0) {
    buf[0] = '\0';
    return 0;
  }
  size_t copy_len = actual_len < buf_size - 1 ? actual_len : buf_size - 1;
  memcpy(buf, t.start, copy_len);
  buf[copy_len] = '\0';
  return copy_len;
}

size_t format_type_to_buf(DataType type, char *buf, size_t size) {
  if (!buf || size == 0)
    return 0;
  size_t offset = 0;

  // Modifiers
  if (type.is_static)
    offset += snprintf(buf + offset, size - offset, "static ");
  if (type.is_mut)
    offset += snprintf(buf + offset, size - offset, "mut ");
  if (type.is_threadlocal)
    offset += snprintf(buf + offset, size - offset, "threadlocal ");
  if (type.is_extern)
    offset += snprintf(buf + offset, size - offset, "extern ");
  if (type.is_async)
    offset += snprintf(buf + offset, size - offset, "async ");

  // Pointers & References
  if (type.ptr_depth != 0) {
    char symbol = (type.ptr_depth > 0) ? '*' : '&';
    int count = (type.ptr_depth > 0) ? type.ptr_depth : -type.ptr_depth;
    for (int i = 0; i < count && offset < size - 1; i++) {
      buf[offset++] = symbol;
    }
  }

  // Base Type Name
  if (type.name.len > 0) {
    offset += snprintf(buf + offset, size - offset, "%.*s", (int)type.name.len,
                       type.name.start);
  }

  // Array Bounds
  for (unsigned int i = 0; i < type.array_dimens; i++) {
    if (type.dim_sizes && type.dim_sizes[i]) {
      AstNode *dim = type.dim_sizes[i];
      if (dim->type == AST_NUM_LIT) {
        offset +=
            snprintf(buf + offset, size - offset, "[%.*s]",
                     (int)dim->as.num_lit.val.len, dim->as.num_lit.val.start);
      } else {
        offset += snprintf(buf + offset, size - offset, "[expr]");
      }
    } else {
      offset += snprintf(buf + offset, size - offset, "[]");
    }
  }
  return offset;
}

void format_func_signature(AstNode *func_node, char *buf, size_t size) {
  if (!func_node || func_node->type != AST_FUNC)
    return;

  size_t offset = 0;

  // Return Type
  offset += format_type_to_buf(func_node->as.func_def.ret_type, buf + offset,
                               size - offset);

  // Name
  offset += snprintf(buf + offset, size - offset, " %.*s(",
                     (int)func_node->as.func_def.fn_name.len,
                     func_node->as.func_def.fn_name.start);

  // Parameters
  AstNode *param = func_node->as.func_def.params;
  while (param && offset < size - 1) {
    offset += format_type_to_buf(param->as.fn_param.type, buf + offset,
                                 size - offset);
    offset +=
        snprintf(buf + offset, size - offset, " %.*s",
                 (int)param->as.fn_param.id.len, param->as.fn_param.id.start);

    if (param->next) {
      offset += snprintf(buf + offset, size - offset, ", ");
    }
    param = param->next;
  }
  snprintf(buf + offset, size - offset, ")");
}

void lsp_send_error(yyjson_val *id_val, int error_code, const char *message) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  if (id_val) {
    yyjson_mut_val *mut_id = yyjson_val_mut_copy(doc, id_val);
    yyjson_mut_obj_add_val(doc, root, "id", mut_id);
  }

  yyjson_mut_val *error_obj = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, error_obj, "code", error_code);
  yyjson_mut_obj_add_str(doc, error_obj, "message", message);
  yyjson_mut_obj_add_val(doc, root, "error", error_obj);

  const char *json_str = yyjson_mut_write(doc, 0, NULL);
  if (json_str) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str),
            json_str);
    fflush(stdout);
    free((void *)json_str);
  }
  yyjson_mut_doc_free(doc);
}

yyjson_mut_doc *lsp_start_response(yyjson_val *id, yyjson_mut_val **root_ptr) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  if (id) {
    yyjson_mut_val *mut_id = yyjson_val_mut_copy(doc, id);
    yyjson_mut_obj_add_val(doc, root, "id", mut_id);
  }
  *root_ptr = root;
  return doc;
}

void lsp_send_doc(yyjson_mut_doc *doc) {
  const char *json_str = yyjson_mut_write(doc, 0, NULL);
  if (json_str) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str),
            json_str);
    fflush(stdout);
    free((void *)json_str);
  }
  yyjson_mut_doc_free(doc);
}

Token get_decl_token(AstNode *node) {
  Token t = {0};
  if (!node)
    return t;
  switch (node->type) {
  case AST_FUNC:
    return node->as.func_def.fn_name;
  case AST_VAR_DECL:
    return node->as.var_decl.id;
  case AST_PARAM:
    return node->as.fn_param.id;
  case AST_STRUCT:
    return node->as.struct_def.structn;
  case AST_UNION:
    return node->as.union_def.unionn;
  case AST_ENUM:
    return node->as.enum_def.enumn;
  case AST_ENUM_MEMBER:
    return node->as.enum_member.name;
  default:
    return t;
  }
}

AstNode *find_ident_at_pos(AstNode *root, unsigned int line, int character) {
  if (!root)
    return NULL;

  size_t cap = 1024;
  AstNode **stack = malloc(sizeof(AstNode *) * cap);
  size_t top = 0;
  stack[top++] = root;

  AstNode *found = NULL;
  unsigned int target_line = line + 1;

  while (top > 0) {
    AstNode *node = stack[--top];
    if (!node)
      continue;

    if (node->type == AST_IDENTIF) {
      Token t = node->as.identif.val;
      if (t.line == target_line && character >= (int)t.col - 1 &&
          character <= (int)(t.col - 1 + t.len)) {
        found = node;
        break;
      }
    } else if (node->type == AST_MEMBER) {
      Token t = node->as.member.name;
      if (t.line == target_line && character >= (int)t.col - 1 &&
          character <= (int)(t.col - 1 + t.len)) {
        found = node;
        break;
      }
    } else if (node->type == AST_USE) {
      Token start = node->as.use_stmt.use_kw;
      Token end = node->as.use_stmt.semicln;
      if (end.len == 0)
        end = node->as.use_stmt.path;
      if (start.line == target_line && character >= (int)start.col - 1 &&
          character <= (int)(end.col - 1 + end.len)) {
        found = node;
        break;
      }
    }

    if (node->next) {
      if (top >= cap - 1) {
        cap *= 2;
        stack = realloc(stack, cap * sizeof(AstNode *));
      }
      stack[top++] = node->next;
    }

#define PUSH_CHILD(n)                                                          \
  do {                                                                         \
    if (n) {                                                                   \
      if (top >= cap - 1) {                                                    \
        cap *= 2;                                                              \
        stack = realloc(stack, cap * sizeof(AstNode *));                       \
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
      PUSH_CHILD(node->as.struct_def.contents);
      break;
    case AST_UNION:
      PUSH_CHILD(node->as.union_def.contents);
      break;
    case AST_ENUM:
      PUSH_CHILD(node->as.enum_def.contents);
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
    default:
      break;
    }
#undef PUSH_CHILD
  }

  free(stack);
  return found;
}

typedef struct {
  AstNode *decl_node;
  const char *fpath;
} ResolvedNode;

ResolvedNode resolve_node_to_decl(AstNode *ident, Arena *tmp_arena) {
  ResolvedNode res = {NULL, NULL};
  if (!ident)
    return res;

  if (ident->type == AST_IDENTIF && ident->as.identif.res_sm) {
    res.decl_node = ident->as.identif.res_sm->decl_node;
    res.fpath = ident->as.identif.res_sm->fpath;
    return res;
  }

  if (ident->type == AST_MEMBER) {
    AstNode *base = ident->as.member.base;
    if (!base)
      return res;

    if (base->type == AST_IDENTIF && base->as.identif.res_sm &&
        base->as.identif.res_sm->is_imported_mod) {
      const char *mod_fpath = base->as.identif.res_sm->fpath;
      char target_uri[8192];
      snprintf(target_uri, sizeof(target_uri), "file://%s", mod_fpath);

      Doc *d =
          (Doc *)map_get(&server_state.docs, target_uri, strlen(target_uri));
      AstNode *ast_root = NULL;

      if (d && d->ast_root) {
        ast_root = d->ast_root;
      } else if (tmp_arena) {
        // Fall back to reading straight off the disk if the document is not
        // open
        ast_root = file_to_ast(tmp_arena, mod_fpath, true);
      }

      if (ast_root) {
        AstNode *stmt = ast_root->as.block.first_stmt;
        while (stmt) {
          Token t = get_decl_token(stmt);
          if (t.len == ident->as.member.name.len &&
              strncmp(t.start, ident->as.member.name.start, t.len) == 0) {
            res.decl_node = stmt;
            res.fpath = mod_fpath;
            return res;
          }
          stmt = stmt->next;
        }
      }
    } else {
      Token ag_name = base->eval_type.name;
      if (ag_name.len > 0) {
        for (size_t i = 0; i < server_state.docs.capacity; i++) {
          HashEntry *entry = server_state.docs.buckets[i];
          while (entry) {
            Doc *d = (Doc *)entry->value;
            if (d->ast_root) {
              AstNode *stmt = d->ast_root->as.block.first_stmt;
              while (stmt) {
                if (stmt->type == AST_STRUCT || stmt->type == AST_UNION ||
                    stmt->type == AST_ENUM) {
                  Token t = get_decl_token(stmt);
                  if (t.len == ag_name.len &&
                      strncmp(t.start, ag_name.start, t.len) == 0) {
                    AstNode *curr = NULL;
                    if (stmt->type == AST_STRUCT)
                      curr = stmt->as.struct_def.contents;
                    else if (stmt->type == AST_UNION)
                      curr = stmt->as.union_def.contents;
                    else if (stmt->type == AST_ENUM)
                      curr = stmt->as.enum_def.contents;

                    while (curr) {
                      Token mt = get_decl_token(curr);
                      if (mt.len == ident->as.member.name.len &&
                          strncmp(mt.start, ident->as.member.name.start,
                                  mt.len) == 0) {
                        res.decl_node = curr;
                        if (strncmp(d->uri, "file://", 7) == 0)
                          res.fpath = d->uri + 7;
                        else
                          res.fpath = d->uri;
                        return res;
                      }
                      curr = curr->next;
                    }
                  }
                }
                stmt = stmt->next;
              }
            }
            entry = entry->next;
          }
        }
      }
    }
  }
  return res;
}

void handle_definition(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  yyjson_val *pos = yyjson_obj_get(params, "position");
  int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
  int character = yyjson_get_int(yyjson_obj_get(pos, "character"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc || !doc->ast_root) {
    goto empty_response;
  }

  Arena tmp_arena = {0};

  AstNode *ident = find_ident_at_pos(doc->ast_root, line, character);
  if (ident && ident->type == AST_USE) {
    Token path_tok = ident->as.use_stmt.path;
    // Extract the path inside the quotes
    if (path_tok.len >= 2 && path_tok.start[0] == '"' &&
        path_tok.start[path_tok.len - 1] == '"') {

      char *rel_path = arena_alloc(&tmp_arena, path_tok.len - 1);
      strncpy(rel_path, path_tok.start + 1, path_tok.len - 2);
      rel_path[path_tok.len - 2] = '\0';

      char *current_abs = absolute_from_uri(uri);
      if (current_abs) {
        char *last_slash = strrchr(current_abs, '/');
        if (last_slash) {
          *last_slash = '\0';
          char full_path[PATH_MAX];
          if (rel_path[0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s", rel_path);
          } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", current_abs,
                     rel_path);
          }
          char *resolved = realpath(full_path, NULL);
          if (resolved) {
            yyjson_mut_val *root;
            yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
            yyjson_mut_val *result = yyjson_mut_obj(jdoc);

            char target_uri[8192];
            snprintf(target_uri, sizeof(target_uri), "file://%s", resolved);
            yyjson_mut_obj_add_str(jdoc, result, "uri", target_uri);

            yyjson_mut_val *range = yyjson_mut_obj(jdoc);
            yyjson_mut_val *start = yyjson_mut_obj(jdoc);
            yyjson_mut_obj_add_int(jdoc, start, "line", 0);
            yyjson_mut_obj_add_int(jdoc, start, "character", 0);
            yyjson_mut_val *end = yyjson_mut_obj(jdoc);
            yyjson_mut_obj_add_int(jdoc, end, "line", 0);
            yyjson_mut_obj_add_int(jdoc, end, "character", 0);
            yyjson_mut_obj_add_val(jdoc, range, "start", start);
            yyjson_mut_obj_add_val(jdoc, range, "end", end);
            yyjson_mut_obj_add_val(jdoc, result, "range", range);
            yyjson_mut_obj_add_val(jdoc, root, "result", result);

            lsp_send_doc(jdoc);
            free(resolved);
            free(current_abs);
            arena_free_all(&tmp_arena);
            return;
          }
        }
        free(current_abs);
      }
    }
    goto empty_response;
  }

  ResolvedNode res = resolve_node_to_decl(ident, &tmp_arena);

  if (res.decl_node) {
    AstNode *decl = res.decl_node;
    Token target_tok = get_decl_token(decl);
    const char *target_fpath = res.fpath;

    if (target_tok.len > 0) {
      yyjson_mut_val *root;
      yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
      yyjson_mut_val *result = yyjson_mut_obj(jdoc);

      char target_uri[8192];
      if (target_fpath) {
        snprintf(target_uri, sizeof(target_uri), "file://%s", target_fpath);
      } else {
        snprintf(target_uri, sizeof(target_uri), "%s", uri);
      }

      yyjson_mut_obj_add_str(jdoc, result, "uri", target_uri);

      yyjson_mut_val *range = yyjson_mut_obj(jdoc);
      yyjson_mut_val *start = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_int(jdoc, start, "line", target_tok.line - 1);
      yyjson_mut_obj_add_int(jdoc, start, "character", target_tok.col);

      yyjson_mut_val *end = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_int(jdoc, end, "line", target_tok.line - 1);
      yyjson_mut_obj_add_int(jdoc, end, "character",
                             target_tok.col + target_tok.len);

      yyjson_mut_obj_add_val(jdoc, range, "start", start);
      yyjson_mut_obj_add_val(jdoc, range, "end", end);
      yyjson_mut_obj_add_val(jdoc, result, "range", range);
      yyjson_mut_obj_add_val(jdoc, root, "result", result);

      lsp_send_doc(jdoc);
      arena_free_all(&tmp_arena);
      return;
    }
  }

  arena_free_all(&tmp_arena);

empty_response: {
  yyjson_mut_val *root;
  yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
  yyjson_mut_obj_add_val(jdoc, root, "result", yyjson_mut_null(jdoc));
  lsp_send_doc(jdoc);
}
}

char *get_comments_above(const char *source, Token target) {
  if (!source || target.line <= 1)
    return NULL;

  const char *ptr = source;
  unsigned int current_line = 1;
  while (*ptr && current_line < target.line) {
    if (*ptr == '\n') {
      current_line++;
    }
    ptr++;
  }
  if (ptr > source && *(ptr - 1) == '\n') {
    ptr--;
  }

  const char *line_end = ptr;

  const char **comment_lines = NULL;
  size_t line_count = 0;
  size_t cap = 0;

  while (ptr > source) {
    while (ptr > source && *(ptr - 1) != '\n') {
      ptr--;
    }
    const char *line_start = (ptr == source && *ptr != '\n') ? source : ptr;
    if (ptr > source) {
      ptr--;
    }

    const char *scan = line_start;
    while (scan < line_end && isspace((unsigned char)*scan)) {
      scan++;
    }

    if (scan + 1 < line_end && scan[0] == '/' && scan[1] == '/') {
      if (line_count + 1 > cap) {
        cap = cap ? cap * 2 : 16;
        comment_lines = realloc(comment_lines, sizeof(const char *) * cap);
        if (!comment_lines)
          return NULL;
      }
      comment_lines[line_count++] = line_start;
    } else {
      break;
    }
  }

  if (line_count == 0) {
    free(comment_lines);
    return NULL;
  }

  size_t total_len = 0;
  for (size_t i = 0; i < line_count; i++) {
    const char *ln_start = comment_lines[i];
    const char *ln_end = (i == 0) ? line_end : comment_lines[i - 1];

    // Skip leading whitespace
    const char *scan = ln_start;
    while (scan < ln_end && isspace((unsigned char)*scan))
      scan++;
    // Find and skip "//"
    if (scan + 1 < ln_end && scan[0] == '/' && scan[1] == '/') {
      scan += 2;
      while (scan < ln_end && isspace((unsigned char)*scan))
        scan++;

      // Find end of line (to strip newline and trailing spaces)
      const char *line_end_no_nl = ln_end;
      while (line_end_no_nl > scan &&
             (line_end_no_nl[-1] == '\n' || line_end_no_nl[-1] == '\r'))
        line_end_no_nl--;
      while (line_end_no_nl > scan &&
             isspace((unsigned char)line_end_no_nl[-1]))
        line_end_no_nl--;

      size_t line_len = line_end_no_nl - scan;
      if (line_len > 0) {
        total_len += line_len;
        if (i < line_count - 1)
          total_len += 1;
      }
    }
  }

  if (total_len == 0) {
    free(comment_lines);
    return NULL;
  }

  char *result = malloc(total_len + 1);
  if (!result) {
    free(comment_lines);
    return NULL;
  }

  char *out = result;
  for (size_t i = 0; i < line_count; i++) {
    const char *ln_start = comment_lines[i];
    const char *ln_end = (i == 0) ? line_end : comment_lines[i - 1];

    const char *scan = ln_start;
    while (scan < ln_end && isspace((unsigned char)*scan))
      scan++;
    if (scan + 1 < ln_end && scan[0] == '/' && scan[1] == '/') {
      scan += 2;
      while (scan < ln_end && isspace((unsigned char)*scan))
        scan++;

      const char *line_end_no_nl = ln_end;
      while (line_end_no_nl > scan &&
             (line_end_no_nl[-1] == '\n' || line_end_no_nl[-1] == '\r'))
        line_end_no_nl--;
      while (line_end_no_nl > scan &&
             isspace((unsigned char)line_end_no_nl[-1]))
        line_end_no_nl--;

      size_t line_len = line_end_no_nl - scan;
      if (line_len > 0) {
        memcpy(out, scan, line_len);
        out += line_len;
        if (i < line_count - 1)
          *out++ = '\n';
      }
    }
  }
  *out = '\0';

  free(comment_lines);
  return result;
}

void handle_hover(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  yyjson_val *pos = yyjson_obj_get(params, "position");
  int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
  int character = yyjson_get_int(yyjson_obj_get(pos, "character"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc || !doc->ast_root) {
    goto empty_response;
  }

  Arena tmp_arena = {0};

  AstNode *ident = find_ident_at_pos(doc->ast_root, line, character);
  ResolvedNode res = resolve_node_to_decl(ident, &tmp_arena);

  if (res.decl_node) {
    AstNode *decl = res.decl_node;
    Token t = get_decl_token(decl);

    if (t.len > 0) {
      char *source_txt = NULL;
      bool allocated_source = false;

      if (res.fpath) {
        char target_uri[8192];
        snprintf(target_uri, sizeof(target_uri), "file://%s", res.fpath);
        Doc *target_doc =
            (Doc *)map_get(&server_state.docs, target_uri, strlen(target_uri));
        if (target_doc) {
          source_txt = target_doc->txt;
        } else {
          // If the destination file isnt open, load text from disk so hover
          // documentation still works
          Doc *target_doc = get_or_load_doc(target_uri, res.fpath);
          if (target_doc) {
            source_txt = target_doc->txt;
          }
        }
      } else {
        source_txt = doc->txt;
      }

      char *comments = get_comments_above(source_txt, t);
      if (allocated_source && source_txt) {
        free(source_txt);
      }

      char signature[8192] = {0};

      if (decl->type == AST_VAR_DECL) {
        snprintf(signature, sizeof(signature), "var %.*s: %.*s", t.len, t.start,
                 decl->as.var_decl.type.name.len,
                 decl->as.var_decl.type.name.start);
      } else if (decl->type == AST_FUNC) {
        char params_buf[4096] = {0};
        size_t offset = 0;
        AstNode *param = decl->as.func_def.params;

        while (param) {
          Token p_id = param->as.fn_param.id;
          Token p_type = param->as.fn_param.type.name;

          int written =
              snprintf(params_buf + offset, sizeof(params_buf) - offset,
                       "%.*s %.*s%s", p_type.len, p_type.start, p_id.len,
                       p_id.start, param->next ? ", " : "");
          if (written > 0) {
            offset += written;
          }
          param = param->next;
        }

        snprintf(signature, sizeof(signature), "%.*s %.*s(%s)",
                 decl->as.func_def.ret_type.name.len,
                 decl->as.func_def.ret_type.name.start, t.len, t.start,
                 params_buf);
      } else if (decl->type == AST_STRUCT) {
        snprintf(signature, sizeof(signature), "struct %.*s", t.len, t.start);
      } else if (decl->type == AST_UNION) {
        snprintf(signature, sizeof(signature), "union %.*s", t.len, t.start);
      } else if (decl->type == AST_ENUM) {
        snprintf(signature, sizeof(signature), "enum %.*s", t.len, t.start);
      } else if (decl->type == AST_ENUM_MEMBER) {
        snprintf(signature, sizeof(signature), "%.*s", t.len, t.start);
      } else {
        snprintf(signature, sizeof(signature), "%.*s", t.len, t.start);
      }

      char md_buffer[16384];
      snprintf(md_buffer, sizeof(md_buffer), "```tereix\n%s\n```\n%s",
               signature, comments ? comments : "");

      if (comments)
        free(comments);

      yyjson_mut_val *root;
      yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
      yyjson_mut_val *result = yyjson_mut_obj(jdoc);

      yyjson_mut_val *contents = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_str(jdoc, contents, "kind", "markdown");
      yyjson_mut_obj_add_str(jdoc, contents, "value", md_buffer);

      yyjson_mut_obj_add_val(jdoc, result, "contents", contents);
      yyjson_mut_obj_add_val(jdoc, root, "result", result);

      lsp_send_doc(jdoc);
      arena_free_all(&tmp_arena);
      return;
    }
  }

  arena_free_all(&tmp_arena);

empty_response: {
  yyjson_mut_val *root;
  yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
  yyjson_mut_obj_add_val(jdoc, root, "result", yyjson_mut_null(jdoc));
  lsp_send_doc(jdoc);
}
}

void add_completion_item(yyjson_mut_doc *jdoc, yyjson_mut_val *arr,
                         const char *label, int kind, const char *detail,
                         const char *documentation, const char *insert_text) {
  yyjson_mut_val *item = yyjson_mut_obj(jdoc);

  yyjson_mut_obj_add_val(jdoc, item, "label", yyjson_mut_strcpy(jdoc, label));
  yyjson_mut_obj_add_int(jdoc, item, "kind", kind);

  if (detail) {
    yyjson_mut_obj_add_val(jdoc, item, "detail",
                           yyjson_mut_strcpy(jdoc, detail));
  }

  // Render attached comments as markdown
  if (documentation) {
    yyjson_mut_val *doc_obj = yyjson_mut_obj(jdoc);
    yyjson_mut_obj_add_str(jdoc, doc_obj, "kind", "markdown");
    yyjson_mut_obj_add_val(jdoc, doc_obj, "value",
                           yyjson_mut_strcpy(jdoc, documentation));
    yyjson_mut_obj_add_val(jdoc, item, "documentation", doc_obj);
  }

  // Provide smart snippets (e.g., auto-insert parens for functions)
  if (insert_text) {
    yyjson_mut_obj_add_val(jdoc, item, "insertText",
                           yyjson_mut_strcpy(jdoc, insert_text));
    yyjson_mut_obj_add_int(jdoc, item, "insertTextFormat",
                           2); // 2 = Snippet format
  }

  yyjson_mut_arr_append(arr, item);
}

void add_local_completions(yyjson_mut_doc *jdoc, yyjson_mut_val *arr,
                           AstNode *func_node, int target_line) {
  if (!func_node || func_node->type != AST_FUNC)
    return;

  AstNode *param = func_node->as.func_def.params;
  while (param && param->type == AST_PARAM) {
    Token id = param->as.fn_param.id;
    if (id.len > 0 && id.line <= (unsigned int)target_line) {
      char name_buf[256];
      snprintf(name_buf, sizeof(name_buf), "%.*s", (int)id.len, id.start);

      char detail_buf[1024] = {0};
      format_type_to_buf(param->as.fn_param.type, detail_buf,
                         sizeof(detail_buf));

      add_completion_item(jdoc, arr, name_buf, 6,
                          detail_buf[0] != '\0' ? detail_buf : "parameter",
                          NULL, NULL);
    }
    param = param->next;
  }

  if (!func_node->as.func_def.block)
    return;

  size_t cap = 256;
  AstNode **stack = malloc(sizeof(AstNode *) * cap);
  size_t top = 0;
  stack[top++] = func_node->as.func_def.block;

  while (top > 0) {
    AstNode *node = stack[--top];
    if (!node)
      continue;

    if (node->type == AST_VAR_DECL) {
      Token id = node->as.var_decl.id;
      // Only suggest variables declared BEFORE or ON the cursor line
      if (id.line <= (unsigned int)target_line) {
        char name_buf[256];
        if (token_to_buf(id, name_buf, sizeof(name_buf)) == 0)
          continue;

        char detail_buf[1024] = {0};
        format_type_to_buf(node->as.var_decl.type, detail_buf,
                           sizeof(detail_buf));

        add_completion_item(
            jdoc, arr, name_buf, 6,
            detail_buf[0] != '\0' ? detail_buf : "local variable", NULL, NULL);
      }
    }

    // Push sibling
    if (node->next) {
      if (top >= cap - 1) {
        cap *= 2;
        stack = realloc(stack, cap * sizeof(AstNode *));
      }
      stack[top++] = node->next;
    }

// Push children based on node type
#define PUSH_CHILD(n)                                                          \
  do {                                                                         \
    if (n) {                                                                   \
      if (top >= cap - 1) {                                                    \
        cap *= 2;                                                              \
        stack = realloc(stack, cap * sizeof(AstNode *));                       \
      }                                                                        \
      stack[top++] = (n);                                                      \
    }                                                                          \
  } while (0)

    if (node->type == AST_BLOCK)
      PUSH_CHILD(node->as.block.first_stmt);
    else if (node->type == AST_IF) {
      PUSH_CHILD(node->as.if_check.elseAct);
      PUSH_CHILD(node->as.if_check.action);
    } else if (node->type == AST_WHILE)
      PUSH_CHILD(node->as.while_loop.action);
    else if (node->type == AST_FOR)
      PUSH_CHILD(node->as.for_loop.action);
    else if (node->type == AST_DEFER)
      PUSH_CHILD(node->as.defer_stmt.contents);
    else if (node->type == AST_SWITCH) {
      PUSH_CHILD(node->as.switch_stmt.default_case);
      PUSH_CHILD(node->as.switch_stmt.cases);
    } else if (node->type == AST_CASE)
      PUSH_CHILD(node->as.case_stmt.action);
#undef PUSH_CHILD
  }
  free(stack);
}

AstNode *find_sue_decl(AstNode *root, const char *target_name,
                       size_t target_len) {
  if (!root)
    return NULL;
  AstNode *stmt = root->as.block.first_stmt;
  while (stmt) {
    if (stmt->type == AST_STRUCT || stmt->type == AST_UNION ||
        stmt->type == AST_ENUM) {
      Token t = get_decl_token(stmt);
      if (t.len == target_len &&
          strncmp(t.start, target_name, target_len) == 0) {
        return stmt;
      }
    }
    stmt = stmt->next;
  }
  return NULL;
}

int get_index_from_pos(const char *txt, int line, int character) {
  if (!txt)
    return 0;
  int cur_l = 0, cur_c = 0, i = 0;
  while (txt[i] != '\0') {
    if (cur_l == line && cur_c == character)
      return i;
    if (txt[i] == '\n') {
      cur_l++;
      cur_c = 0;
    } else
      cur_c++;
    i++;
  }
  return i; // end of string
}

void get_pos_from_index(const char *txt, int index, int *line, int *character) {
  *line = 0;
  *character = 0;
  for (int i = 0; i < index && txt[i] != '\0'; i++) {
    if (txt[i] == '\n') {
      (*line)++;
      *character = 0;
    } else {
      (*character)++;
    }
  }
}

bool split_qualified_type(Token qualified, Token *mod_alias,
                          Token *simple_name) {
  if (!qualified.start || qualified.len == 0)
    return false;

  // Find last dot in the token
  const char *dot = memchr(qualified.start, '.', qualified.len);
  if (!dot)
    return false;

  size_t alias_len = dot - qualified.start;
  size_t name_len = qualified.len - alias_len - 1;

  mod_alias->start = qualified.start;
  mod_alias->len = alias_len;
  mod_alias->type = TOKEN_IDENTIF;

  simple_name->start = dot + 1;
  simple_name->len = name_len;
  simple_name->type = TOKEN_IDENTIF;

  return true;
}

void handle_completion(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  yyjson_val *pos = yyjson_obj_get(params, "position");
  int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
  int character = yyjson_get_int(yyjson_obj_get(pos, "character"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc || !doc->ast_root) {
    goto empty_response;
  }

  yyjson_mut_val *root;
  yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
  yyjson_mut_val *result = yyjson_mut_arr(jdoc);

  AstNode *containing_func = NULL;
  AstNode *stmt = doc->ast_root->as.block.first_stmt;
  while (stmt) {
    if (stmt->type == AST_FUNC &&
        stmt->as.func_def.fn_name.line <= (unsigned int)(line + 1)) {
      containing_func = stmt;
    }
    stmt = stmt->next;
  }

  int cursor_idx = get_index_from_pos(doc->txt, line, character);
  int p = cursor_idx - 1;

  // Skip current word being typed
  while (p >= 0 && (isalnum((unsigned char)doc->txt[p]) || doc->txt[p] == '_'))
    p--;
  // Skip whitespace
  while (p >= 0 && isspace((unsigned char)doc->txt[p]))
    p--;

  bool is_dot_trigger = (p >= 0 && doc->txt[p] == '.');

  if (is_dot_trigger) {
    p--;
    while (p >= 0 && isspace((unsigned char)doc->txt[p]))
      p--;

    unsigned int ident_end = p;
    while (p >= 0 &&
           (isalnum((unsigned char)doc->txt[p]) || doc->txt[p] == '_'))
      p--;
    unsigned int ident_start = p + 1;
    unsigned int ident_len = ident_end - ident_start + 1;

    if (ident_len > 0) {
      char base_name[256];
      snprintf(base_name, sizeof(base_name), "%.*s", (int)ident_len,
               &doc->txt[ident_start]);

      bool is_module_access = false;
      AstNode *mod_ast = NULL;
      Arena tmp_arena = {0};

      AstNode *top_stmt = doc->ast_root->as.block.first_stmt;
      while (top_stmt) {
        if (top_stmt->type == AST_USE) {
          char mod_name[256] = {0};
          extract_use_namespace(top_stmt, mod_name, sizeof(mod_name));

          if (strlen(mod_name) == ident_len &&
              strncmp(base_name, mod_name, ident_len) == 0) {
            is_module_access = true;

            // Resolve the path and grab the AST for this module
            Token pt = top_stmt->as.use_stmt.path;
            if (pt.len >= 2) {
              char rel_path[PATH_MAX];
              strncpy(rel_path, pt.start + 1, pt.len - 2);
              rel_path[pt.len - 2] = '\0';

              char *current_abs = absolute_from_uri(uri);
              if (current_abs) {
                char *last_slash = strrchr(current_abs, '/');
                if (last_slash)
                  *last_slash = '\0';

                char full_path[PATH_MAX];
                if (rel_path[0] == '/')
                  snprintf(full_path, sizeof(full_path), "%s", rel_path);
                else
                  snprintf(full_path, sizeof(full_path), "%s/%s", current_abs,
                           rel_path);

                char *resolved = realpath(full_path, NULL);
                if (resolved) {
                  char target_uri[8192];
                  snprintf(target_uri, sizeof(target_uri), "file://%s",
                           resolved);
                  Doc *imported_doc = get_or_load_doc(target_uri, resolved);
                  if (imported_doc && imported_doc->ast_root) {
                    mod_ast = imported_doc->ast_root;
                  }
                  free(resolved);
                }
                free(current_abs);
              }
            }
            break;
          }
        }
        top_stmt = top_stmt->next;
      }

      if (is_module_access) {
        if (mod_ast) {
          AstNode *ext_stmt = mod_ast->as.block.first_stmt;
          while (ext_stmt) {
            Token t = get_decl_token(ext_stmt);
            if (t.len > 0) {
              char m_name[256];
              snprintf(m_name, sizeof(m_name), "%.*s", (int)t.len, t.start);

              char detail_buf[1024] = {0};
              char insert_buf[256] = {0};
              int kind = 1; // Default

              if (ext_stmt->type == AST_FUNC) {
                kind = 3; // Function
                format_func_signature(ext_stmt, detail_buf, sizeof(detail_buf));
                snprintf(insert_buf, sizeof(insert_buf), "%s($1)", m_name);
              } else if (ext_stmt->type == AST_VAR_DECL) {
                kind = 6; // Variable
                format_type_to_buf(ext_stmt->as.var_decl.type, detail_buf,
                                   sizeof(detail_buf));
              } else if (ext_stmt->type == AST_STRUCT ||
                         ext_stmt->type == AST_UNION) {
                kind = 22; // Struct
                snprintf(detail_buf, sizeof(detail_buf), "%s",
                         ext_stmt->type == AST_STRUCT ? "struct" : "union");
              } else if (ext_stmt->type == AST_ENUM) {
                kind = 13; // Enum
                snprintf(detail_buf, sizeof(detail_buf), "enum");
              }
              add_completion_item(jdoc, result, m_name, kind,
                                  detail_buf[0] != '\0' ? detail_buf : NULL,
                                  NULL,
                                  insert_buf[0] != '\0' ? insert_buf : NULL);
            }
            ext_stmt = ext_stmt->next;
          }
        }
        arena_free_all(&tmp_arena);
        yyjson_mut_obj_add_val(jdoc, root, "result", result);
        lsp_send_doc(jdoc);
        return;
      }

      Token type_name = {0};
      bool found_type = false;

      // Look for the variable whose type we need
      if (containing_func) {
        AstNode *param = containing_func->as.func_def.params;
        while (param) {
          if (param->as.fn_param.id.len == ident_len &&
              strncmp(param->as.fn_param.id.start, base_name, ident_len) == 0) {
            type_name = param->as.fn_param.type.name;
            found_type = true;
            break;
          }
          param = param->next;
        }

        if (!found_type && containing_func->as.func_def.block) {
          size_t cap = 256;
          AstNode **stack = malloc(sizeof(AstNode *) * cap);
          size_t top = 0;
          stack[top++] = containing_func->as.func_def.block;

          while (top > 0) {
            AstNode *node = stack[--top];
            if (!node)
              continue;

            if (node->type == AST_VAR_DECL) {
              Token vid = node->as.var_decl.id;
              if (vid.line <= (unsigned int)(line + 1) &&
                  vid.len == ident_len &&
                  strncmp(vid.start, base_name, ident_len) == 0) {
                type_name = node->as.var_decl.type.name;
                found_type = true;
                break;
              }
            }
            // Push sibling
            if (node->next) {
              if (top >= cap - 1) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof(AstNode *));
              }
              stack[top++] = node->next;
            }
            // Push children
            if (node->type == AST_BLOCK && node->as.block.first_stmt) {
              if (top >= cap - 1) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof(AstNode *));
              }
              stack[top++] = node->as.block.first_stmt;
            } else if (node->type == AST_IF) {
              if (node->as.if_check.elseAct) {
                if (top >= cap - 1) {
                  cap *= 2;
                  stack = realloc(stack, cap * sizeof(AstNode *));
                }
                stack[top++] = node->as.if_check.elseAct;
              }
              if (node->as.if_check.action) {
                if (top >= cap - 1) {
                  cap *= 2;
                  stack = realloc(stack, cap * sizeof(AstNode *));
                }
                stack[top++] = node->as.if_check.action;
              }
            } else if (node->type == AST_WHILE && node->as.while_loop.action) {
              if (top >= cap - 1) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof(AstNode *));
              }
              stack[top++] = node->as.while_loop.action;
            } else if (node->type == AST_FOR && node->as.for_loop.action) {
              if (top >= cap - 1) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof(AstNode *));
              }
              stack[top++] = node->as.for_loop.action;
            }
          }
          free(stack);
        }
      }

      // Search Global Variables
      if (!found_type) {
        AstNode *gst = doc->ast_root->as.block.first_stmt;
        while (gst) {
          if (gst->type == AST_VAR_DECL) {
            if (gst->as.var_decl.id.len == ident_len &&
                strncmp(gst->as.var_decl.id.start, base_name, ident_len) == 0) {
              type_name = gst->as.var_decl.type.name;
              found_type = true;
              break;
            }
          }
          gst = gst->next;
        }
      }

      // If we found a type name, resolve the sue declaration
      AstNode *type_decl = NULL;
      const char *decl_src_txt = doc->txt;

      if (found_type && type_name.len > 0) {
        Token mod_alias = {0}, simple_name = {0};
        bool is_qualified =
            split_qualified_type(type_name, &mod_alias, &simple_name);

        if (is_qualified) {
          // Qualified type is "ModuleAlias.TypeName"
          // Find the use statement that matches the module alias
          AstNode *use_stmt = doc->ast_root->as.block.first_stmt;
          while (use_stmt && !type_decl) {
            if (use_stmt->type == AST_USE) {
              char mod_name[256] = {0};
              extract_use_namespace(use_stmt, mod_name, sizeof(mod_name));
              if (strlen(mod_name) == mod_alias.len &&
                  strncmp(mod_name, mod_alias.start, mod_alias.len) == 0) {
                Token pt = use_stmt->as.use_stmt.path;
                if (pt.len >= 2) {
                  char rel_path[PATH_MAX];
                  strncpy(rel_path, pt.start + 1, pt.len - 2);
                  rel_path[pt.len - 2] = '\0';
                  char *current_abs = absolute_from_uri(uri);
                  if (current_abs) {
                    char *last_slash = strrchr(current_abs, '/');
                    if (last_slash)
                      *last_slash = '\0';
                    char full_path[PATH_MAX];
                    if (rel_path[0] == '/')
                      snprintf(full_path, sizeof(full_path), "%s", rel_path);
                    else
                      snprintf(full_path, sizeof(full_path), "%s/%s",
                               current_abs, rel_path);
                    char *resolved = realpath(full_path, NULL);
                    if (resolved) {
                      char target_uri[8192];
                      snprintf(target_uri, sizeof(target_uri), "file://%s",
                               resolved);
                      Doc *imported_doc = (Doc *)map_get(
                          &server_state.docs, target_uri, strlen(target_uri));
                      if (imported_doc && imported_doc->ast_root) {
                        type_decl =
                            find_sue_decl(imported_doc->ast_root,
                                          simple_name.start, simple_name.len);
                        if (type_decl)
                          decl_src_txt = imported_doc->txt;
                      } else {
                        AstNode *mod_ast =
                            file_to_ast(&tmp_arena, resolved, true);
                        if (mod_ast) {
                          type_decl = find_sue_decl(mod_ast, simple_name.start,
                                                    simple_name.len);
                          if (type_decl) {
                            // Load source for doc comments
                            FILE *f = fopen(resolved, "rb");
                            if (f) {
                              fseek(f, 0, SEEK_END);
                              long flen = ftell(f);
                              fseek(f, 0, SEEK_SET);
                              if (flen >= 0) {
                                char *stxt = arena_alloc(&tmp_arena, flen + 1);
                                size_t read_bytes = fread(stxt, 1, flen, f);
                                if (read_bytes == (size_t)flen) {
                                  stxt[flen] = '\0';
                                  decl_src_txt = stxt;
                                } else {
                                  decl_src_txt = NULL;
                                }
                              }
                              fclose(f);
                            }
                          }
                        }
                      }
                      free(resolved);
                    }
                    free(current_abs);
                  }
                }
                break;
              }
            }
            use_stmt = use_stmt->next;
          }
        } else {
          // Current then imported
          type_decl =
              find_sue_decl(doc->ast_root, type_name.start, type_name.len);
          if (!type_decl) {
            // Search through use statements
            AstNode *use_stmt = doc->ast_root->as.block.first_stmt;
            while (use_stmt && !type_decl) {
              if (use_stmt->type == AST_USE) {
                Token pt = use_stmt->as.use_stmt.path;
                if (pt.len >= 2) {
                  char rel_path[PATH_MAX];
                  strncpy(rel_path, pt.start + 1, pt.len - 2);
                  rel_path[pt.len - 2] = '\0';
                  char *current_abs = absolute_from_uri(uri);
                  if (current_abs) {
                    char *last_slash = strrchr(current_abs, '/');
                    if (last_slash)
                      *last_slash = '\0';
                    char full_path[PATH_MAX];
                    if (rel_path[0] == '/')
                      snprintf(full_path, sizeof(full_path), "%s", rel_path);
                    else
                      snprintf(full_path, sizeof(full_path), "%s/%s",
                               current_abs, rel_path);
                    char *resolved = realpath(full_path, NULL);
                    if (resolved) {
                      char target_uri[8192];
                      snprintf(target_uri, sizeof(target_uri), "file://%s",
                               resolved);
                      Doc *imported_doc = (Doc *)map_get(
                          &server_state.docs, target_uri, strlen(target_uri));
                      if (imported_doc && imported_doc->ast_root) {
                        type_decl =
                            find_sue_decl(imported_doc->ast_root,
                                          type_name.start, type_name.len);
                        if (type_decl)
                          decl_src_txt = imported_doc->txt;
                      } else {
                        AstNode *mod_ast =
                            file_to_ast(&tmp_arena, resolved, true);
                        if (mod_ast) {
                          type_decl = find_sue_decl(mod_ast, type_name.start,
                                                    type_name.len);
                          if (type_decl) {
                            FILE *f = fopen(resolved, "rb");
                            if (f) {
                              fseek(f, 0, SEEK_END);
                              long flen = ftell(f);
                              fseek(f, 0, SEEK_SET);
                              if (flen >= 0) {
                                char *stxt = arena_alloc(&tmp_arena, flen + 1);
                                size_t read_bytes = fread(stxt, 1, flen, f);
                                if (read_bytes == (size_t)flen) {
                                  stxt[flen] = '\0';
                                  decl_src_txt = stxt;
                                } else {
                                  decl_src_txt = NULL;
                                }
                              }
                              fclose(f);
                            }
                          }
                        }
                      }
                      free(resolved);
                    }
                    free(current_abs);
                  }
                }
              }
              use_stmt = use_stmt->next;
            }
          }
        }
      }

      // Populate completions if we found a sue declaration
      if (type_decl) {
        AstNode *member =
            (type_decl->type == AST_STRUCT)  ? type_decl->as.struct_def.contents
            : (type_decl->type == AST_UNION) ? type_decl->as.union_def.contents
            : (type_decl->type == AST_ENUM)  ? type_decl->as.enum_def.contents
                                             : NULL;

        while (member) {
          Token mt = get_decl_token(member);
          if (mt.len == 0) {
            member = member->next;
            continue;
          }
          char m_name[256] = {0};
          if (token_to_buf(mt, m_name, sizeof(m_name)) == 0) {
            member = member->next;
            continue;
          }

          char detail_buf[1024] = {0};
          char insert_buf[256] = {0};

          char *docs =
              decl_src_txt ? get_comments_above(decl_src_txt, mt) : NULL;
          int kind = 5; // default to field

          if (member->type == AST_FUNC) {
            kind = 2; // method
            format_func_signature(member, detail_buf, sizeof(detail_buf));
            snprintf(insert_buf, sizeof(insert_buf), "%s($1)", m_name);
          } else if (member->type == AST_VAR_DECL) {
            format_type_to_buf(member->as.var_decl.type, detail_buf,
                               sizeof(detail_buf));
          } else if (member->type == AST_ENUM_MEMBER) {
            kind = 20; // enum member
            snprintf(detail_buf, sizeof(detail_buf), "enum member");
          } else if (member->type == AST_STRUCT || member->type == AST_UNION ||
                     member->type == AST_ENUM) {
            kind = (member->type == AST_ENUM) ? 13 : 22;
            snprintf(detail_buf, sizeof(detail_buf), "nested %s",
                     member->type == AST_STRUCT
                         ? "struct"
                         : (member->type == AST_UNION ? "union" : "enum"));
          }

          add_completion_item(jdoc, result, m_name, kind,
                              detail_buf[0] != '\0' ? detail_buf : NULL, docs,
                              insert_buf[0] != '\0' ? insert_buf : NULL);

          if (docs)
            free(docs);
          member = member->next;
        }
        arena_free_all(&tmp_arena);
        yyjson_mut_obj_add_val(jdoc, root, "result", result);
        lsp_send_doc(jdoc);
        return;
      }
      arena_free_all(&tmp_arena);
    }
  } else {
    for (size_t i = 0; i < kwlistlen; i++)
      add_completion_item(jdoc, result, kwlist[i], 14, "keyword", NULL, NULL);

    for (size_t i = 0; i < typelistlen; i++)
      add_completion_item(jdoc, result, typelist[i], 14, "type", NULL, NULL);

    AstNode *gst = doc->ast_root->as.block.first_stmt;
    while (gst) {
      Token t = get_decl_token(gst);
      if (t.len > 0) {
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "%.*s", t.len, t.start);

        char detail_buf[1024] = {0};
        char insert_buf[256] = {0};
        char *docs = get_comments_above(doc->txt, t);

        int kind = 1;
        if (gst->type == AST_FUNC) {
          kind = 3;
          format_func_signature(gst, detail_buf, sizeof(detail_buf));
          if (gst->as.func_def.params) {
            snprintf(insert_buf, sizeof(insert_buf), "%s($1)", name_buf);
          } else {
            snprintf(insert_buf, sizeof(insert_buf), "%s()", name_buf);
          }
        } else if (gst->type == AST_VAR_DECL) {
          kind = 6;
          format_type_to_buf(gst->as.var_decl.type, detail_buf,
                             sizeof(detail_buf));
        } else if (gst->type == AST_STRUCT || gst->type == AST_UNION) {
          kind = 22;
          snprintf(detail_buf, sizeof(detail_buf), "%s",
                   gst->type == AST_STRUCT ? "struct" : "union");
        } else if (gst->type == AST_ENUM) {
          kind = 13;
          snprintf(detail_buf, sizeof(detail_buf), "enum");
        }

        add_completion_item(jdoc, result, name_buf, kind,
                            detail_buf[0] != '\0' ? detail_buf : NULL, docs,
                            insert_buf[0] != '\0' ? insert_buf : NULL);
        if (docs)
          free(docs);
      }
      gst = gst->next;
    }

    AstNode *use_stmt = doc->ast_root->as.block.first_stmt;
    while (use_stmt) {
      if (use_stmt->type == AST_USE) {
        char mod_name[256] = {0};
        extract_use_namespace(use_stmt, mod_name, sizeof(mod_name));
        add_completion_item(jdoc, result, mod_name, 9, "module",
                            "Imported namespace", NULL);
      }
      use_stmt = use_stmt->next;
    }

    if (containing_func) {
      const char *local_kws[] = {
          "if",   "else",  "while",    "for",    "ret",  "defer", "switch",
          "case", "break", "continue", "sizeof", "true", "false", "null"};
      for (size_t i = 0; i < sizeof(local_kws) / sizeof(local_kws[0]); i++) {
        add_completion_item(jdoc, result, local_kws[i], 14, "keyword", NULL,
                            NULL);
      }
      add_local_completions(jdoc, result, containing_func, line + 1);
    }
  }

  yyjson_mut_obj_add_val(jdoc, root, "result", result);
  lsp_send_doc(jdoc);
  return;

empty_response: {
  yyjson_mut_val *rt;
  yyjson_mut_doc *jd = lsp_start_response(id, &rt);
  yyjson_mut_obj_add_val(jd, rt, "result", yyjson_mut_null(jd));
  lsp_send_doc(jd);
}
}

void handle_document_symbols(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc || !doc->ast_root) {
    yyjson_mut_val *root;
    yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
    yyjson_mut_obj_add_val(jdoc, root, "result", yyjson_mut_null(jdoc));
    lsp_send_doc(jdoc);
    return;
  }

  yyjson_mut_val *root;
  yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
  yyjson_mut_val *result = yyjson_mut_arr(jdoc);

  AstNode *stmt = doc->ast_root->as.block.first_stmt;
  while (stmt) {
    Token t = get_decl_token(stmt);
    if (t.len > 0) {
      yyjson_mut_val *symbol = yyjson_mut_obj(jdoc);

      char name_buf[256];
      snprintf(name_buf, sizeof(name_buf), "%.*s", (int)t.len, t.start);

      yyjson_mut_obj_add_val(jdoc, symbol, "name",
                             yyjson_mut_strcpy(jdoc, name_buf));

      // LSP SymbolKinds Function=12, Variable=13, Struct=23, Enum=10
      int kind = 13;
      if (stmt->type == AST_FUNC)
        kind = 12;
      else if (stmt->type == AST_STRUCT)
        kind = 23;
      else if (stmt->type == AST_ENUM)
        kind = 10;
      yyjson_mut_obj_add_int(jdoc, symbol, "kind", kind);

      // Range where the symbol exists in the file
      yyjson_mut_val *range = yyjson_mut_obj(jdoc);
      yyjson_mut_val *start = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_int(jdoc, start, "line", t.line - 1);
      yyjson_mut_obj_add_int(jdoc, start, "character", t.col);
      yyjson_mut_val *end = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_int(jdoc, end, "line", t.line - 1);
      yyjson_mut_obj_add_int(jdoc, end, "character", t.col + t.len);
      yyjson_mut_obj_add_val(jdoc, range, "start", start);
      yyjson_mut_obj_add_val(jdoc, range, "end", end);

      yyjson_mut_obj_add_val(jdoc, symbol, "range", range);
      yyjson_mut_obj_add_val(jdoc, symbol, "selectionRange", range);

      yyjson_mut_arr_append(result, symbol);
    }
    stmt = stmt->next;
  }

  yyjson_mut_obj_add_val(jdoc, root, "result", result);
  lsp_send_doc(jdoc);
}

void handle_initialize(yyjson_val *params, yyjson_val *id) {
  (void)params; // No use currently
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = lsp_start_response(id, &root);

  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_val *capabilities = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, capabilities, "textDocumentSync", 1);
  yyjson_mut_obj_add_bool(doc, capabilities, "definitionProvider", true);
  yyjson_mut_obj_add_bool(doc, capabilities, "hoverProvider", true);
  yyjson_mut_obj_add_bool(doc, capabilities, "documentSymbolProvider", true);
  yyjson_mut_val *comp_options = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_bool(doc, comp_options, "resolveProvider", false);
  yyjson_mut_val *trigger_chars = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_str(doc, trigger_chars, ".");
  yyjson_mut_obj_add_val(doc, comp_options, "triggerCharacters", trigger_chars);
  yyjson_mut_val *sig_help = yyjson_mut_obj(doc);
  yyjson_mut_val *sig_trigger = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_str(doc, sig_trigger, "(");
  yyjson_mut_arr_add_str(doc, sig_trigger, ",");
  yyjson_mut_obj_add_val(doc, sig_help, "triggerCharacters", sig_trigger);
  yyjson_mut_val *retrigger = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_str(doc, retrigger, ",");
  yyjson_mut_obj_add_val(doc, sig_help, "retriggerCharacters", retrigger);
  yyjson_mut_obj_add_val(doc, capabilities, "signatureHelpProvider", sig_help);
  yyjson_mut_obj_add_val(doc, capabilities, "completionProvider", comp_options);
  yyjson_mut_obj_add_val(doc, result, "capabilities", capabilities);
  yyjson_mut_obj_add_val(doc, root, "result", result);

  lsp_send_doc(doc);
}

void handle_shutdown(yyjson_val *id) {
  server_state.state = SHUTDOWN;
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = lsp_start_response(id, &root);
  yyjson_mut_obj_add_val(doc, root, "result", yyjson_mut_null(doc));
  lsp_send_doc(doc);
}

void handle_exit() {
  if (server_state.state == SHUTDOWN) {
    exit(0);
  } else {
    exit(1);
  }
}

void publish_diagnostics_from_list(const char *uri, DiagList *diags) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  yyjson_mut_obj_add_str(doc, root, "method",
                         "textDocument/publishDiagnostics");

  yyjson_mut_val *params = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_str(doc, params, "uri", uri);

  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  for (size_t i = 0; i < diags->count; i++) {
    Diag *d = &diags->items[i];
    yyjson_mut_val *diag_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, diag_obj, "severity", d->severity);
    yyjson_mut_obj_add_str(doc, diag_obj, "message", d->message);

    yyjson_mut_val *range = yyjson_mut_obj(doc);
    yyjson_mut_val *start = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, start, "line", d->start_line);
    yyjson_mut_obj_add_int(doc, start, "character", d->start_char);
    yyjson_mut_val *end = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, end, "line", d->end_line);
    yyjson_mut_obj_add_int(doc, end, "character", d->end_char);
    yyjson_mut_obj_add_val(doc, range, "start", start);
    yyjson_mut_obj_add_val(doc, range, "end", end);
    yyjson_mut_obj_add_val(doc, diag_obj, "range", range);

    yyjson_mut_arr_append(arr, diag_obj);
  }

  yyjson_mut_obj_add_val(doc, params, "diagnostics", arr);
  yyjson_mut_obj_add_val(doc, root, "params", params);

  const char *json_str = yyjson_mut_write(doc, 0, NULL);
  if (json_str) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str),
            json_str);
    fflush(stdout);
    free((void *)json_str);
  }
  yyjson_mut_doc_free(doc);
}

void compile_doc(Doc *doc) {
  if (doc->ast_arena) {
    arena_free_all(doc->ast_arena);
    free(doc->ast_arena);
    doc->ast_arena = NULL;
    doc->ast_root = NULL;
  }

  doc->ast_arena = malloc(sizeof(Arena));
  *doc->ast_arena = (Arena){0};

  DiagList diags;
  diaglist_init(&diags, 1024);
  char *abspath = absolute_from_uri(doc->uri);
  if (!abspath) {
    DiagList empty = {0};
    publish_diagnostics_from_list(doc->uri, &empty);
    return;
  }

  AstNode *root = str_to_ast(doc->ast_arena, doc->txt, abspath, &diags, true);

  if (root) {
    SemCtx sem = {0};
    sem_init(&sem, doc->ast_arena);
    sem.diags = &diags;

    Worklist pending = {0};
    wl_push(&pending, abspath);

    const char *current_path;
    while ((current_path = wl_pop(&pending)) != NULL) {
      const char *curr_abs = resolve_alloc(doc->ast_arena, current_path);
      if (!curr_abs || map_get(&sem.mod_cache, curr_abs, strlen(curr_abs)))
        continue;

      AstNode *ast = NULL;

      if (strcmp(curr_abs, abspath) == 0) {
        ast = root;
      } else {
        char uri_buf[8192];
        snprintf(uri_buf, sizeof(uri_buf), "file://%s", curr_abs);
        Doc *dep = get_or_load_doc(uri_buf, curr_abs);
        if (dep) {
          ast = dep->ast_root;
        }
      }

      if (!ast)
        continue;

      const char *mod_name = extract_mod_name(doc->ast_arena, curr_abs);
      Module *mod = new_mod(doc->ast_arena, curr_abs, mod_name, ast);

      if (strcmp(curr_abs, abspath) == 0)
        sem_main_mod = mod;

      map_set(&sem.mod_cache, curr_abs, strlen(curr_abs), mod);

      // Push extracted dependencies back to the worklist
      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        if (stmt->type == AST_USE) {
          size_t path_len = stmt->as.use_stmt.path.len;
          if (path_len > 2) {
            char *clean_rel = arena_alloc(doc->ast_arena, path_len - 1);
            strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
            clean_rel[path_len - 2] = '\0';
            wl_push(&pending, clean_rel);
          }
        }
        stmt = stmt->next;
      }
    }

    resolve_imports(doc->ast_arena, &sem);

    for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
      HashEntry *entry = sem.mod_cache.buckets[i];
      while (entry) {
        Module *mod = (Module *)entry->value;
        sem_current_mod = mod;
        collect_mod_symbols(doc->ast_arena, mod, &sem);
        entry = entry->next;
      }
    }

    if (sem_main_mod) {
      ScopeStack ss;
      scope_stack_init(&ss, doc->ast_arena);

      sem_current_mod = sem_main_mod;
      resolve_scopes(doc->ast_arena, sem_main_mod, &ss, &sem);

      sem_current_mod = sem_main_mod;
      type_check_ast(doc->ast_arena, sem_main_mod->ast_root, &sem);
    }

    doc->ast_root = root; // Cache the primary AST for Hover/Go-To definitions

    if (pending.paths) {
      free((void *)pending.paths);
    }
    sem_deinit(&sem);
  }

  publish_diagnostics_from_list(doc->uri, &diags);
  diaglist_free(&diags);
  free(abspath);
}

void handle_did_save(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (doc)
    compile_doc(doc);
}

void handle_did_open(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  const char *text = yyjson_get_str(yyjson_obj_get(text_doc, "text"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  Doc *existing = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (existing) {
    // If it was already cached as a dependency, just update its text
    free(existing->txt);
    existing->txt = strdup(text);
    existing->version = version;
    compile_doc(existing);
    return;
  }

  Doc *doc = malloc(sizeof(Doc));
  doc->uri = strdup(uri);
  doc->txt = strdup(text);
  doc->version = version;
  doc->ast_arena = NULL;
  doc->ast_root = NULL;

  map_set(&server_state.docs, doc->uri, strlen(doc->uri), doc);
  server_state.doc_count++;

  compile_doc(doc);
}

void handle_did_change(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc)
    return;
  if (version <= doc->version)
    return;

  // Because textDocumentSync is 1 (Full), contentChanges is an array
  // where the first element contains the full updated text.
  yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
  yyjson_val *first_change = yyjson_arr_get(changes, 0);
  const char *new_text = yyjson_get_str(yyjson_obj_get(first_change, "text"));

  free(doc->txt); // Free the old text buffer
  doc->txt = strdup(new_text);
  doc->version = version;
  compile_doc(doc);
}

void handle_did_close(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));

  // We leave the Doc entirely intact in the map as a dependency cache
  DiagList empty = {0};
  publish_diagnostics_from_list(uri, &empty);
}

void handle_signature_help(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  yyjson_val *pos = yyjson_obj_get(params, "position");
  int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
  int character = yyjson_get_int(yyjson_obj_get(pos, "character"));

  Doc *doc = (Doc *)map_get(&server_state.docs, uri, strlen(uri));
  if (!doc || !doc->txt)
    goto empty_response;

  int cursor_idx = get_index_from_pos(doc->txt, line, character);
  int active_param = 0;
  int depth = 0;
  int p = cursor_idx - 1;

  while (p >= 0) {
    char c = doc->txt[p];
    if (c == '"') {
      p--;
      while (p >= 0) {
        if (doc->txt[p] == '"' && (p == 0 || doc->txt[p - 1] != '\\'))
          break;
        p--;
      }
    } else if (c == '\'') {
      p--;
      while (p >= 0) {
        if (doc->txt[p] == '\'' && (p == 0 || doc->txt[p - 1] != '\\'))
          break;
        p--;
      }
    } else if (c == ')') {
      depth++;
    } else if (c == '(') {
      depth--;
      if (depth < 0)
        break;
    } else if (c == ',' && depth == 0) {
      active_param++;
    }
    p--;
  }

  if (p < 0)
    goto empty_response;

  p--;
  while (p >= 0 && isspace((unsigned char)doc->txt[p]))
    p--;
  if (p < 0 || (!isalnum((unsigned char)doc->txt[p]) && doc->txt[p] != '_'))
    goto empty_response;

  unsigned int ident_end = p;
  while (p >= 0 && (isalnum((unsigned char)doc->txt[p]) || doc->txt[p] == '_'))
    p--;
  unsigned int ident_start = p + 1;
  unsigned int ident_len = ident_end - ident_start + 1;

  int ident_line, ident_char;
  get_pos_from_index(doc->txt, ident_start, &ident_line, &ident_char);

  Arena tmp_arena = {0};
  ResolvedNode res = {NULL, NULL};

  if (doc->ast_root) {
    AstNode *ident = find_ident_at_pos(doc->ast_root, ident_line, ident_char);
    res = resolve_node_to_decl(ident, &tmp_arena);
  }

  if (!res.decl_node) {
    const char *target_name = &doc->txt[ident_start];

    // Attempt to resolve module access if a dot is before the identifier
    int temp_p = ident_start - 1;
    while (temp_p >= 0 && isspace((unsigned char)doc->txt[temp_p]))
      temp_p--;

    if (temp_p >= 0 && doc->txt[temp_p] == '.') {
      temp_p--;
      while (temp_p >= 0 && isspace((unsigned char)doc->txt[temp_p]))
        temp_p--;
      unsigned int mod_end = temp_p;
      while (temp_p >= 0 && (isalnum((unsigned char)doc->txt[temp_p]) ||
                             doc->txt[temp_p] == '_'))
        temp_p--;
      unsigned int mod_start = temp_p + 1;
      unsigned int mod_len = mod_end - mod_start + 1;

      if (mod_len > 0) {
        char mod_name[256];
        snprintf(mod_name, sizeof(mod_name), "%.*s", (int)mod_len,
                 &doc->txt[mod_start]);

        AstNode *top_stmt = doc->ast_root->as.block.first_stmt;
        while (top_stmt) {
          if (top_stmt->type == AST_USE) {
            char extract_name[256] = {0};
            extract_use_namespace(top_stmt, extract_name, sizeof(extract_name));

            if (strlen(extract_name) == mod_len &&
                strncmp(mod_name, extract_name, mod_len) == 0) {
              Token pt = top_stmt->as.use_stmt.path;
              if (pt.len >= 2) {
                char rel_path[PATH_MAX];
                strncpy(rel_path, pt.start + 1, pt.len - 2);
                rel_path[pt.len - 2] = '\0';

                char *current_abs = absolute_from_uri(uri);
                if (current_abs) {
                  char *last_slash = strrchr(current_abs, '/');
                  if (last_slash)
                    *last_slash = '\0';

                  char full_path[PATH_MAX];
                  if (rel_path[0] == '/')
                    snprintf(full_path, sizeof(full_path), "%s", rel_path);
                  else
                    snprintf(full_path, sizeof(full_path), "%s/%s", current_abs,
                             rel_path);

                  char *resolved = realpath(full_path, NULL);
                  if (resolved) {
                    char target_uri[8192];
                    snprintf(target_uri, sizeof(target_uri), "file://%s",
                             resolved);
                    AstNode *mod_ast = NULL;
                    Doc *imported_doc = get_or_load_doc(target_uri, resolved);
                    if (imported_doc && imported_doc->ast_root) {
                      mod_ast = imported_doc->ast_root;
                    }

                    if (mod_ast) {
                      AstNode *mod_stmt = mod_ast->as.block.first_stmt;
                      while (mod_stmt) {
                        if (mod_stmt->type == AST_FUNC) {
                          Token t = mod_stmt->as.func_def.fn_name;
                          if (t.len == ident_len &&
                              strncmp(t.start, target_name, ident_len) == 0) {
                            res.decl_node = mod_stmt;
                            free(resolved);
                            free(current_abs);
                            goto fallback_found;
                          }
                        }
                        mod_stmt = mod_stmt->next;
                      }
                    }
                    free(resolved);
                  }
                  free(current_abs);
                }
              }
              break;
            }
          }
          top_stmt = top_stmt->next;
        }
      }
    }

    // Local/non-module functions
    if (!res.decl_node) {
      for (size_t i = 0; i < server_state.docs.capacity; i++) {
        HashEntry *entry = server_state.docs.buckets[i];
        while (entry) {
          Doc *d = (Doc *)entry->value;
          if (d->ast_root) {
            AstNode *stmt = d->ast_root->as.block.first_stmt;
            while (stmt) {
              if (stmt->type == AST_FUNC) {
                Token t = stmt->as.func_def.fn_name;
                if (t.len == ident_len &&
                    strncmp(t.start, target_name, ident_len) == 0) {
                  res.decl_node = stmt;
                  goto fallback_found;
                }
              }
              stmt = stmt->next;
            }
          }
          entry = entry->next;
        }
      }
    }
  }

fallback_found:
  if (res.decl_node && res.decl_node->type == AST_FUNC) {
    AstNode *decl = res.decl_node;
    Token fn_name = decl->as.func_def.fn_name;
    Token ret_type = decl->as.func_def.ret_type.name;

    yyjson_mut_val *root;
    yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
    yyjson_mut_val *result = yyjson_mut_obj(jdoc);
    yyjson_mut_val *signatures = yyjson_mut_arr(jdoc);
    yyjson_mut_val *sig_obj = yyjson_mut_obj(jdoc);
    yyjson_mut_val *params_arr = yyjson_mut_arr(jdoc);

    char sig_label[4096] = {0};

    int offset = snprintf(sig_label, sizeof(sig_label), "%.*s(", fn_name.len,
                          fn_name.start);

    AstNode *param = decl->as.func_def.params;
    while (param) {
      char param_label[256];
      Token p_id = param->as.fn_param.id;
      Token p_type = param->as.fn_param.type.name;

      snprintf(param_label, sizeof(param_label), "%.*s %.*s", p_type.len,
               p_type.start, p_id.len, p_id.start);

      int start_idx = offset;
      int written = snprintf(sig_label + offset, sizeof(sig_label) - offset,
                             "%s", param_label);
      if (written > 0)
        offset += written;
      int end_idx = offset;

      yyjson_mut_val *p_obj = yyjson_mut_obj(jdoc);
      yyjson_mut_val *label_arr = yyjson_mut_arr(jdoc);

      yyjson_mut_arr_add_int(jdoc, label_arr, start_idx);
      yyjson_mut_arr_add_int(jdoc, label_arr, end_idx);
      yyjson_mut_obj_add_val(jdoc, p_obj, "label", label_arr);

      yyjson_mut_arr_append(params_arr, p_obj);

      if (param->next) {
        written =
            snprintf(sig_label + offset, sizeof(sig_label) - offset, ", ");
        if (written > 0)
          offset += written;
      }
      param = param->next;
    }

    snprintf(sig_label + offset, sizeof(sig_label) - offset, "): %.*s",
             ret_type.len, ret_type.start);

    yyjson_mut_obj_add_str(jdoc, sig_obj, "label", sig_label);
    yyjson_mut_obj_add_val(jdoc, sig_obj, "parameters", params_arr);
    yyjson_mut_arr_append(signatures, sig_obj);

    yyjson_mut_obj_add_val(jdoc, result, "signatures", signatures);
    yyjson_mut_obj_add_int(jdoc, result, "activeSignature", 0);
    yyjson_mut_obj_add_int(jdoc, result, "activeParameter", active_param);
    yyjson_mut_obj_add_val(jdoc, root, "result", result);

    lsp_send_doc(jdoc);
    arena_free_all(&tmp_arena);
    return;
  }

  arena_free_all(&tmp_arena);

empty_response: {
  yyjson_mut_val *root;
  yyjson_mut_doc *jdoc = lsp_start_response(id, &root);
  yyjson_mut_obj_add_val(jdoc, root, "result", yyjson_mut_null(jdoc));
  lsp_send_doc(jdoc);
}
}

void start_lsp_server() {
  // Disable stdout buffering so messages go straight to the editor
  setvbuf(stdout, NULL, _IONBF, 0);
  Arena lsp_arena = {0};
  map_init(&server_state.docs, &lsp_arena, 64);
  server_state.state = UNINITIALIZED;

  while (1) {
    int content_length = 0;
    char buffer[256];

    // Read headers until we hit the empty line "\r\n"
    while (fgets(buffer, sizeof(buffer), stdin)) {
      if (strncmp(buffer, "Content-Length: ", 16) == 0) {
        content_length = atoi(buffer + 16);
      }
      if (strcmp(buffer, "\r\n") == 0) {
        break;
      }
    }

    if (content_length <= 0)
      continue;

    char *payload = malloc(content_length + 1);
    size_t read_bytes = fread(payload, 1, content_length, stdin);
    if (read_bytes != (size_t)content_length) {
      free(payload);
      continue;
    }
    payload[content_length] = '\0';

    yyjson_doc *doc = yyjson_read(payload, strlen(payload), 0);
    if (doc) {
      yyjson_val *root = yyjson_doc_get_root(doc);
      const char *method = yyjson_get_str(yyjson_obj_get(root, "method"));

      yyjson_val *id = yyjson_obj_get(root, "id");

      if (method) {
        if (strcmp(method, "initialize") == 0) {
          handle_initialize(yyjson_obj_get(root, "params"), id);
          server_state.state = INITIALIZED;
        } else if (strcmp(method, "shutdown") == 0) {
          handle_shutdown(id);
        } else if (strcmp(method, "exit") == 0) {
          handle_exit();
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
          handle_did_open(yyjson_obj_get(root, "params"));
          // lsp_publish_diagnostics(uri); // Compile and check on open
        } else if (strcmp(method, "textDocument/didChange") == 0) {
          handle_did_change(yyjson_obj_get(root, "params"));
          // lsp_publish_diagnostics(uri); // Compile and check on change
        } else if (strcmp(method, "textDocument/didClose") == 0) {
          handle_did_close(yyjson_obj_get(root, "params"));
        } else if (strcmp(method, "textDocument/didSave") == 0) {
          handle_did_save(yyjson_obj_get(root, "params"));
        } else if (strcmp(method, "textDocument/definition") == 0) {
          handle_definition(yyjson_obj_get(root, "params"), id);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
          handle_document_symbols(yyjson_obj_get(root, "params"), id);
        } else if (strcmp(method, "textDocument/hover") == 0) {
          handle_hover(yyjson_obj_get(root, "params"), id);
        } else if (strcmp(method, "textDocument/completion") == 0) {
          handle_completion(yyjson_obj_get(root, "params"), id);
        } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
          handle_signature_help(yyjson_obj_get(root, "params"), id);
        } else {
          if (id) {
            // MethodNotFound
            lsp_send_error(id, -32601, "Method not implemented");
          }
        }
      }

      yyjson_doc_free(doc);
    }
    free(payload);
  }
  arena_free_all(&lsp_arena);
}
