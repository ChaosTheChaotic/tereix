#ifndef LSP_H
#define LSP_H

#include "hashmap.h"
#include "yyjson.h"
#include <stdbool.h>

typedef struct {
  char *uri;
  char *txt;
  unsigned int version;
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
