#ifndef LEX_TYPES_H
#define LEX_TYPES_H

#include "hashmap.h"

typedef struct {
  char *start;
  char *curr;
  unsigned int line;
  unsigned int col;

  HashMap kw_map;
  HashMap op_map;
  HashMap comp_map;
  HashMap type_kw_map;
} LexCtx;


#endif // !LEX_TYPES_H
