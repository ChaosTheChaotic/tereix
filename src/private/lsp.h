#ifndef LSP_H
#define LSP_H

#include "hashmap.h"
#include "ast_types.h"
#include "yyjson.h"
#include <stdbool.h>

typedef struct {
  char *uri;
  char *txt;
  int version;

  Arena *ast_arena;
  AstNode *ast_root;
} Doc;

typedef struct {
  enum {
    UNINITIALIZED,
    INITIALIZING,
    INITIALIZED,
    SHUTDOWN,
  } state;
  HashMap open_docs;
  unsigned int doc_count;
  yyjson_doc *capabilities;
  const char *root_uri;
} LspState;

void start_lsp_server();

#endif // !LSP_H
