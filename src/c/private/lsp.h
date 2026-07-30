#ifndef LSP_H
#define LSP_H

#include "ast_types.h"
#include "diag.h"
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
  DiagList diags;
} Doc;

typedef struct {
  enum {
    UNINITIALIZED,
    INITIALIZING,
    INITIALIZED,
    SHUTDOWN,
  } state;
  HashMap docs;
  unsigned int doc_count;
  yyjson_doc *capabilities;
  const char *root_uri;
  SemCtx proj_sem;
} LspState;

typedef struct {
  uint32_t line;
  uint32_t character;
} LspPosition;

typedef struct {
  LspPosition start;
  LspPosition end;
} LspRange;

typedef struct {
  const char *uri;
  LspRange range;
} LspLocation;

typedef struct {
  const char *contents; // Simplified Hover representation
} LspHover;

typedef struct {
  LspRange range;
  int severity; // 1 = Error, 2 = Warning, 3 = Info, 4 = Hint
  const char *code;
  const char *source;
  const char *message;
} LspDiagnostic;

void start_lsp_server();

#endif // !LSP_H
