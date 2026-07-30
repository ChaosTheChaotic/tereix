#include "thread_pool.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

void *worker(void *arg) {
  ThreadPool *tp = arg;
  while (1) {
    pthread_mutex_lock(&tp->lock);
    while (tp->head == NULL && !tp->shutdown) {
      pthread_cond_wait(&tp->cond, &tp->lock);
    }
    if (tp->shutdown && tp->head == NULL) {
      pthread_mutex_unlock(&tp->lock);
      break;
    }
    Task *task = tp->head;
    tp->head = task->next;
    if (tp->head == NULL)
      tp->tail = NULL;
    pthread_mutex_unlock(&tp->lock);

    bool ok = task->func(task->data);
    (void)ok; // could collect errors
    free(task);
    atomic_fetch_sub(&tp->pending, 1);
    pthread_cond_broadcast(&tp->done_cond);
  }
  return NULL;
}

ThreadPool *tp_create(size_t num_threads) {
  ThreadPool *tp = calloc(1, sizeof(ThreadPool));
  pthread_mutex_init(&tp->lock, NULL);
  pthread_cond_init(&tp->cond, NULL);
  pthread_cond_init(&tp->done_cond, NULL);
  atomic_init(&tp->pending, 0);
  tp->num_threads = num_threads;
  tp->threads = malloc(num_threads * sizeof(pthread_t));
  for (size_t i = 0; i < num_threads; i++) {
    pthread_create(&tp->threads[i], NULL, worker, tp);
  }
  return tp;
}

void tp_destroy(ThreadPool *tp) {
  pthread_mutex_lock(&tp->lock);
  tp->shutdown = true;
  pthread_cond_broadcast(&tp->cond);
  pthread_mutex_unlock(&tp->lock);
  for (size_t i = 0; i < tp->num_threads; i++) {
    pthread_join(tp->threads[i], NULL);
  }
  pthread_mutex_destroy(&tp->lock);
  pthread_cond_destroy(&tp->cond);
  pthread_cond_destroy(&tp->done_cond);
  free(tp->threads);
  free(tp);
}

void tp_submit(ThreadPool *tp, TaskFunc func, void *data) {
  Task *task = malloc(sizeof(Task));
  task->func = func;
  task->data = data;
  task->next = NULL;
  pthread_mutex_lock(&tp->lock);
  if (tp->tail)
    tp->tail->next = task;
  else
    tp->head = task;
  tp->tail = task;
  atomic_fetch_add(&tp->pending, 1);
  pthread_cond_signal(&tp->cond);
  pthread_mutex_unlock(&tp->lock);
}

void tp_wait(ThreadPool *tp) {
  pthread_mutex_lock(&tp->lock);
  while (atomic_load(&tp->pending) > 0) {
    pthread_cond_wait(&tp->done_cond, &tp->lock);
  }
  pthread_mutex_unlock(&tp->lock);
}
