#ifndef LSP_H
#define LSP_H

#include "ast_types.h"
#include "hashmap.h"
#include "sem_types.h"
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
  SemCtx proj_sem;
} LspState;

void start_lsp_server();

#endif // !LSP_H
