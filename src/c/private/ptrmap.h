#ifndef PTRMAP_H
#define PTRMAP_H

#include "ast_types.h"

typedef struct {
  AstNode *ptr;
  uint32_t idx;
} PtrMapEntry;

typedef struct {
  PtrMapEntry *entries;
  uint32_t cap;
  uint32_t count;
} PtrMap;

void ptrmap_init(PtrMap *map, uint32_t cap);
void ptrmap_expand(PtrMap *map);
bool ptrmap_put(PtrMap *map, AstNode *ptr, uint32_t idx);
uint32_t ptrmap_get(PtrMap *map, AstNode *ptr);

#endif // !PTRMAP_H
