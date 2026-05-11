#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LspState server_state = {.state = UNINITIALIZED};

void lsp_send_error(yyjson_mut_val *id_val, int error_code,
                    const char *message) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  if (id_val)
    yyjson_mut_obj_add_val(doc, root, "id", id_val);

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

void lsp_send_response(yyjson_mut_val *result_val, yyjson_mut_val *id_val) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  if (id_val)
    yyjson_mut_obj_add_val(doc, root, "id", id_val);
  if (result_val)
    yyjson_mut_obj_add_val(doc, root, "result", result_val);

  const char *json_str = yyjson_mut_write(doc, 0, NULL);
  if (json_str) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json_str),
            json_str);
    fflush(stdout);
    free((void *)json_str);
  }
  yyjson_mut_doc_free(doc);
}

void handle_initialize(yyjson_val *params, yyjson_mut_val *id) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_val *capabilities = yyjson_mut_obj(doc);

  yyjson_mut_obj_add_int(doc, capabilities, "textDocumentSync", 1); // Maybe later allow support for incrememntal?
  yyjson_mut_obj_add_bool(doc, capabilities, "hoverProvider", true);
  yyjson_mut_obj_add_bool(doc, capabilities, "definitionProvider", true);

  yyjson_mut_obj_add_val(doc, result, "capabilities", capabilities);
  lsp_send_response(result, id);
  yyjson_mut_doc_free(doc);
}

void handle_shutdown(yyjson_mut_val *id) {
  server_state.state = SHUTDOWN;

  // The spec requires returning null for the shutdown response
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  lsp_send_response(yyjson_mut_null(doc), id);
  yyjson_mut_doc_free(doc);
}

void handle_exit() {
  if (server_state.state == SHUTDOWN) {
    exit(0);
  } else {
    exit(1);
  }
}

void handle_did_open(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  const char *text = yyjson_get_str(yyjson_obj_get(text_doc, "text"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  // Allocate and store the document in your LspState hashmap
  Doc *doc = malloc(sizeof(Doc));
  doc->uri = strdup(uri);
  doc->txt = strdup(text);
  doc->version = version;

  // Assuming you initialize server_state.open_docs somewhere
  map_set(&server_state.open_docs, doc->uri, strlen(doc->uri), doc);
  server_state.doc_count++;
}

void handle_did_change(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));
  int version = yyjson_get_int(yyjson_obj_get(text_doc, "version"));

  // Because textDocumentSync is 1 (Full), contentChanges is an array
  // where the first element contains the full updated text.
  yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
  yyjson_val *first_change = yyjson_arr_get(changes, 0);
  const char *new_text = yyjson_get_str(yyjson_obj_get(first_change, "text"));

  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
  if (doc) {
    free(doc->txt); // Free the old text buffer
    doc->txt = strdup(new_text);
    doc->version = version;

    // TODO: This is where you trigger your compiler's lexer/parser
    // using the in-memory doc->txt to generate fresh diagnostics!
  }
}

// Note: You will eventually want to pass a struct/array of actual errors here
// instead of hardcoding one, but this shows the required yyjson structure.
void lsp_publish_diagnostics(const char *uri) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
  yyjson_mut_obj_add_str(doc, root, "method",
                         "textDocument/publishDiagnostics");

  yyjson_mut_val *params = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_str(doc, params, "uri", uri);

  yyjson_mut_val *diagnostics_arr = yyjson_mut_arr(doc);

  yyjson_mut_val *diag = yyjson_mut_obj(doc);

  // 1 = Error, 2 = Warning, 3 = Info, 4 = Hint
  yyjson_mut_obj_add_int(doc, diag, "severity", 1);
  yyjson_mut_obj_add_str(doc, diag, "message",
                         "Expected ';' at end of declaration");

  // Lexer uses 1 indexed lines but LSP uses 0 indexed
  yyjson_mut_val *range = yyjson_mut_obj(doc);
  yyjson_mut_val *start = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, start, "line", 10 /* lex_line - 1 */);
  yyjson_mut_obj_add_int(doc, start, "character", 5 /* lex_col - 1 */);

  yyjson_mut_val *end = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, end, "line", 10 /* lex_line - 1 */);
  yyjson_mut_obj_add_int(
      doc, end, "character",
      6 /* lex_col - 1 */); // usually 1 char wide for simple errors

  yyjson_mut_obj_add_val(doc, range, "start", start);
  yyjson_mut_obj_add_val(doc, range, "end", end);
  yyjson_mut_obj_add_val(doc, diag, "range", range);

  yyjson_mut_arr_append(diagnostics_arr, diag);
  yyjson_mut_obj_add_val(doc, params, "diagnostics", diagnostics_arr);
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

void handle_did_close(yyjson_val *params) {
  yyjson_val *text_doc = yyjson_obj_get(params, "textDocument");
  const char *uri = yyjson_get_str(yyjson_obj_get(text_doc, "uri"));

  // Remove from map and free memory to prevent leaks
  Doc *doc = (Doc *)map_get(&server_state.open_docs, uri, strlen(uri));
  if (doc) {
    // You'll need a map_remove or map_delete implementation in your hashmap.h
    // map_delete(&server_state.open_docs, uri, strlen(uri));
    free(doc->uri);
    free(doc->txt);
    free(doc);
    server_state.doc_count--;
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

      yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
      yyjson_mut_val *id =
          yyjson_val_mut_copy(mut_doc, yyjson_obj_get(root, "id"));

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
        } else {
          // Handle unimplemented methods
          if (id) {
            // MethodNotFound
            lsp_send_error(id, -32601, "Method not implemented");
          }
        }
      }

      yyjson_mut_doc_free(mut_doc);
      yyjson_doc_free(doc);
    }
    free(payload);
  }
}
