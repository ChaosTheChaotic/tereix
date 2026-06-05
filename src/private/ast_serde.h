#ifndef AST_SERDE_H
#define AST_SERDE_H

#include "ast_types.h"

typedef struct {
  AstNode *node;
  int depth;
  const char *label;
} AstPrintItem;

typedef struct DepNode {
  Token name;
  struct DepNode *next;
} DepNode;

typedef struct DeclMetadata {
  Token name;
  ASTN_TYPE type;
  const char *src_start;
  const char *src_end;
  uint32_t node_hash;

  DepNode *calls_to;
  DepNode *uses_types;

  bool is_dirty;
  struct DeclMetadata *next;
} DeclMetadata;

AstNode *cache_read_ast(Arena *arena, const char *cache_path,
                        const char *source_base);

void cache_write_ast(const char *cache_path, AstNode *root,
                     const char *source_base);


DeclMetadata *cache_read_decl_meta(Arena *arena, const char *path, const char *src_base);

void cache_write_decl_meta(const char *path, DeclMetadata *meta, const char *src_base);

void propagate_declaration_invalidation(DeclMetadata *old_cached_decls, DeclMetadata *new_parsed_decls);

DeclMetadata *analyze_module_declarations(Arena *arena, AstNode *module_root);

#endif // !AST_SERDE_H
