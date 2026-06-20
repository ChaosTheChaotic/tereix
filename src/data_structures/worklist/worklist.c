#include "worklist.h"

void wl_init(Worklist *wl) {
  wl->paths = NULL;
  wl->count = 0;
  wl->capacity = 0;
#ifdef ENABLE_THREADS
  pthread_mutex_init(&wl->mutex, NULL);
  pthread_cond_init(&wl->cond, NULL);
  wl->done = 0;
#endif
}

void wl_push(Worklist *wl, const char *path) {
#ifdef ENABLE_THREADS
  pthread_mutex_lock(&wl->mutex);
#endif
  if (wl->count >= wl->capacity) {
    wl->capacity = wl->capacity ? wl->capacity * 2 : 32;
    wl->paths = realloc(wl->paths, wl->capacity * sizeof(const char *));
  }
  wl->paths[wl->count++] = path;
#ifdef ENABLE_THREADS
  pthread_cond_signal(&wl->cond);
  pthread_mutex_unlock(&wl->mutex);
#endif
}

const char *wl_pop(Worklist *wl) {
#ifdef ENABLE_THREADS
  pthread_mutex_lock(&wl->mutex);
  while (wl->count == 0 && !wl->done) {
    pthread_cond_wait(&wl->cond, &wl->mutex);
  }
  if (wl->count == 0) {
    pthread_mutex_unlock(&wl->mutex);
    return NULL;
  }
  const char *path = wl->paths[--wl->count];
  pthread_mutex_unlock(&wl->mutex);
  return path;
#else
  if (wl->count == 0)
    return NULL;
  return wl->paths[--wl->count];
#endif
}

void wl_done(Worklist *wl) {
#ifdef ENABLE_THREADS
  pthread_mutex_lock(&wl->mutex);
  wl->done = 1;
  pthread_cond_broadcast(&wl->cond);
  pthread_mutex_unlock(&wl->mutex);
#endif
}

void wl_destroy(Worklist *wl) {
#ifdef ENABLE_THREADS
  pthread_mutex_destroy(&wl->mutex);
  pthread_cond_destroy(&wl->cond);
#endif
  free(wl->paths);
}
