#ifndef WORKLIST_H
#define WORKLIST_H

#include <stdlib.h>

typedef struct {
  const char **paths;
  size_t count;
  size_t capacity;
} Worklist;

void wl_push(Worklist *wl, const char *path);
const char *wl_pop(Worklist *wl);

#endif // !WORKLIST_H
