#ifndef AST_SERDE_H
#define AST_SERDE_H

#include "ast_types.h"

AstNode *cache_read_ast(Arena *arena, const char *cache_path,
                        const char *source_base);

void cache_write_ast(const char *cache_path, AstNode *root,
                     const char *source_base);

#endif // !AST_SERDE_H
