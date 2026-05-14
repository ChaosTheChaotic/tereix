#include "lsp.h"
#include "arena.h"
#include "ast_types.h"
#include "diag.h"
#include "sem_types.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LspState server_state = {.state = UNINITIALIZED};

void lsp_send_error(yyjson_val *id_val, int error_code,
                    const char *message) {
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
        fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str), json_str);
        fflush(stdout);
        free((void *)json_str);
    }
    yyjson_mut_doc_free(doc);
}

void handle_initialize(yyjson_val *params, yyjson_val *id) {
	(void)params; // No use currently
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = lsp_start_response(id, &root);

  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_val *capabilities = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, capabilities, "textDocumentSync", 1);
  yyjson_mut_obj_add_bool(doc, capabilities, "hoverProvider", false);
  yyjson_mut_obj_add_bool(doc, capabilities, "definitionProvider", false);
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
    SemCtx sem = {.arena = doc->ast_arena, .diags = &diags};
    Module mod = {
        .abs_path = abspath, .mod_name = "lsp_temp", .ast_root = root};
    map_init(&mod.local_symbols, doc->ast_arena, 64);
    map_init(&mod.imported_mods, doc->ast_arena, 8);
    AstNode *stmt = root->as.block.first_stmt;
    while (stmt) {
      if (stmt->type == AST_USE) {
        if (stmt->as.use_stmt.alias.len > 0) {
          // If aliased, register the alias
          map_set(&mod.imported_mods, stmt->as.use_stmt.alias.start,
                  stmt->as.use_stmt.alias.len, (void *)1);
        } else if (stmt->as.use_stmt.path.len > 2) {
          // Extract base module name from the path string
          char *clean_rel =
              arena_alloc(doc->ast_arena, stmt->as.use_stmt.path.len - 1);
          strncpy(clean_rel, stmt->as.use_stmt.path.start + 1,
                  stmt->as.use_stmt.path.len - 2);
          clean_rel[stmt->as.use_stmt.path.len - 2] = '\0';

          const char *base = strrchr(clean_rel, '/');
          base = base ? base + 1 : clean_rel;

          const char *ext = strrchr(base, '.');
          size_t key_len = ext ? (size_t)(ext - base) : strlen(base);

          // Insert dummy value
					// resolve_scopes only checks the key presence
          map_set(&mod.imported_mods, base, key_len, (void *)1);
        }
      }
      stmt = stmt->next;
    }
    ScopeStack ss;
    scope_stack_init(&ss, doc->ast_arena);
    resolve_scopes(doc->ast_arena, &mod, &ss, &sem);
    type_check_ast(doc->ast_arena, root, &sem);
    doc->ast_root = root; // cache the AST
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

      yyjson_val *id =
          yyjson_obj_get(root, "id");

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
        } else {
          // Handle unimplemented methods
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
