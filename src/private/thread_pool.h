#ifndef THREAD_POOL_H

#define THREAD_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

typedef bool (*TaskFunc)(void *data);

typedef struct Task {
    TaskFunc func;
    void *data;
    struct Task *next;
} Task;

typedef struct ThreadPool {
    Task *head, *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_cond_t done_cond;
    atomic_size_t pending;
    bool shutdown;
    size_t num_threads;
    pthread_t *threads;
} ThreadPool;

ThreadPool* tp_create(size_t num_threads);
void tp_destroy(ThreadPool *tp);

void tp_submit(ThreadPool *tp, TaskFunc func, void *data);

void tp_wait(ThreadPool *tp);


#endif // !THREAD_POOL_H
