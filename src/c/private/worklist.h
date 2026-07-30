#ifndef WORKLIST_H
#define WORKLIST_H

#include <stdlib.h>
#ifdef ENABLE_THREADS
#include <pthread.h>
#endif

typedef struct {
  const char **paths;
  size_t count;
  size_t capacity;
#ifdef ENABLE_THREADS
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  unsigned short int done; // 0 = running, 1 = no more items will be added
#endif
} Worklist;

void wl_init(Worklist *wl);
void wl_push(Worklist *wl, const char *path);
const char *wl_pop(Worklist *wl); // blocks until an item is available or done
void wl_done(Worklist *wl);       // signal that production is finished
void wl_destroy(Worklist *wl);

#endif
