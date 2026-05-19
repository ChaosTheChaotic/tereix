#include "worklist.h"

void wl_push(Worklist *wl, const char *path) {
  if (wl->count >= wl->capacity) {
    wl->capacity = (wl->capacity == 0) ? 32 : wl->capacity * 2;
    wl->paths = realloc((void *)wl->paths, sizeof(const char *) * wl->capacity);
  }
  wl->paths[wl->count++] = path;
}

const char *wl_pop(Worklist *wl) {
  if (wl->count == 0)
    return NULL;
  return wl->paths[--wl->count];
}

