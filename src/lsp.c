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
      if (t.line == target_line && character >= (int)t.col &&
          character <= (int)(t.col + t.len)) {
        found = node;
        break;
      }
    } else if (node->type == AST_MEMBER) {
      Token t = node->as.member.name;
      if (t.line == target_line && character >= (int)t.col &&
          character <= (int)(t.col + t.len)) {
        found = node;
        break;
      }
    } else if (node->type == AST_USE) {
      Token start = node->as.use_stmt.use_kw;
      Token end = node->as.use_stmt.semicln;
      if (end.len == 0)
        end = node->as.use_stmt.path;
      if (start.line == target_line && character >= (int)start.col - 1 &&
          character <= (int)(end.col + end.len)) {
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

      Doc *d = (Doc *)map_get(&server_state.open_docs, target_uri,
                              strlen(target_uri));
      AstNode *ast_root = NULL;

      if (d && d->ast_root) {
        ast_root = d->ast_root;
      } else if (tmp_arena) {
        // Fall back to reading straight off the disk if the document is not
        // open
        ast_root = file_to_ast(tmp_arena, mod_fpath);
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
        for (size_t i = 0; i < server_state.open_docs.capacity; i++) {
          HashEntry *entry = server_state.open_docs.buckets[i];
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

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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
        Doc *target_doc = (Doc *)map_get(&server_state.open_docs, target_uri,
                                         strlen(target_uri));
        if (target_doc) {
          source_txt = target_doc->txt;
        } else {
          // If the destination file isnt open, load text from disk so hover
          // documentation still works
          FILE *f = fopen(res.fpath, "rb");
          if (f) {
            fseek(f, 0, SEEK_END);
            long flen = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (flen >= 0) {
              source_txt = malloc(flen + 1);
              if (source_txt) {
                size_t read_bytes = fread(source_txt, 1, flen, f);
                source_txt[read_bytes] = '\0';
                allocated_source = true;
              }
            }
            fclose(f);
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

void handle_completion(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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
      yyjson_mut_val *item = yyjson_mut_obj(jdoc);

      char name_buf[256];
      snprintf(name_buf, sizeof(name_buf), "%.*s", t.len, t.start);
      yyjson_mut_obj_add_str(jdoc, item, "label", name_buf);

      int kind = 1;
      if (stmt->type == AST_FUNC)
        kind = 3;
      else if (stmt->type == AST_VAR_DECL)
        kind = 6;
      else if (stmt->type == AST_STRUCT)
        kind = 22;
      else if (stmt->type == AST_ENUM)
        kind = 13;
      else if (stmt->type == AST_UNION)
        kind = 22;

      yyjson_mut_obj_add_int(jdoc, item, "kind", kind);
      yyjson_mut_arr_append(result, item);
    }
    stmt = stmt->next;
  }

  yyjson_mut_obj_add_val(jdoc, root, "result", result);
  lsp_send_doc(jdoc);
}

void handle_document_symbols(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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
      snprintf(name_buf, sizeof(name_buf), "%.*s", t.len, t.start);
      yyjson_mut_obj_add_str(jdoc, symbol, "name", name_buf);

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

  AstNode *root = str_to_ast(doc->ast_arena, doc->txt, abspath, &diags);

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
        // Get deps currently open in editor
        char uri_buf[8192];
        snprintf(uri_buf, sizeof(uri_buf), "file://%s", curr_abs);
        Doc *open_doc =
            (Doc *)map_get(&server_state.open_docs, uri_buf, strlen(uri_buf));

        if (open_doc) {
          // Pass null for diags to avoid displaying external syntax errors in
          // the current file
          ast = str_to_ast(doc->ast_arena, open_doc->txt, curr_abs, NULL);
        } else {
          // Fall back to reading straight off the disk
          ast = file_to_ast(doc->ast_arena, curr_abs);
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

    ScopeStack ss;
    scope_stack_init(&ss, doc->ast_arena);

    for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
      HashEntry *entry = sem.mod_cache.buckets[i];
      while (entry) {
        Module *mod = (Module *)entry->value;
        ss.count = 0;
        sem_current_mod = mod;
        resolve_scopes(doc->ast_arena, mod, &ss, &sem);
        entry = entry->next;
      }
    }

    for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
      HashEntry *entry = sem.mod_cache.buckets[i];
      while (entry) {
        Module *mod = (Module *)entry->value;
        sem_current_mod = mod;
        type_check_ast(doc->ast_arena, mod->ast_root, &sem);
        entry = entry->next;
      }
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
  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
  if (doc)
    compile_doc(doc);
}

void handle_did_open(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  const char *text = yyjson_get_str(yyjson_obj_get(text_doc, "text"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  Doc *existing = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
  if (existing) {
    // same cleanup as handle_did_close, but without publishing empty
    // diagnostics
    map_remove(&server_state.open_docs, uri, strlen(uri));
    if (existing->ast_arena) {
      arena_free_all(existing->ast_arena);
      free(existing->ast_arena);
    }
    free(existing->uri);
    free(existing->txt);
    free(existing);
    server_state.doc_count--;
  }

  // Allocate and store the document in your LspState hashmap
  Doc *doc = malloc(sizeof(Doc));
  doc->uri = strdup(uri);
  doc->txt = strdup(text);
  doc->version = version;
  doc->ast_arena = NULL;
  doc->ast_root = NULL;

  // Assuming you initialize server_state.open_docs somewhere
  map_set(&server_state.open_docs, doc->uri, strlen(doc->uri), doc);
  server_state.doc_count++;

  compile_doc(doc);
}

void handle_did_change(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
  if (doc) {
    map_remove(&server_state.open_docs, uri, strlen(uri));
    if (doc->ast_arena) {
      arena_free_all(doc->ast_arena);
      free(doc->ast_arena);
    }
    free(doc->uri);
    free(doc->txt);
    free(doc);
    server_state.doc_count--;
  }
  DiagList empty = {0};
  publish_diagnostics_from_list(uri, &empty);
}

int get_index_from_pos(const char *txt, int line, int character) {
  int cur_l = 0, cur_c = 0, i = 0;
  while (txt[i] != '\0') {
    if (cur_l == line && cur_c == character) return i;
    if (txt[i] == '\n') { cur_l++; cur_c = 0; } 
    else { cur_c++; }
    i++;
  }
  return i;
}

void get_pos_from_index(const char *txt, int index, int *line, int *character) {
  *line = 0; *character = 0;
  for (int i = 0; i < index && txt[i] != '\0'; i++) {
    if (txt[i] == '\n') { (*line)++; *character = 0; } 
    else { (*character)++; }
  }
}

void handle_signature_help(yyjson_val *params, yyjson_val *id) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  yyjson_val *pos = yyjson_obj_get(params, "position");
  int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
  int character = yyjson_get_int(yyjson_obj_get(pos, "character"));

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
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
    for (size_t i = 0; i < server_state.open_docs.capacity; i++) {
      HashEntry *entry = server_state.open_docs.buckets[i];
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

      yyjson_mut_val *p_obj = yyjson_mut_obj(jdoc);
      yyjson_mut_obj_add_str(jdoc, p_obj, "label", param_label);
      yyjson_mut_arr_append(params_arr, p_obj);

      offset += snprintf(sig_label + offset, sizeof(sig_label) - offset, "%s%s",
                         param_label, param->next ? ", " : "");
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
  map_init(&server_state.open_docs, &lsp_arena, 64);
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
