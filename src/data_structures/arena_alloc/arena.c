#include "arena.h"
#include "parse_types.h"
#include <string.h>

// Align the pointer to the nearest 8 bytes
static size_t align_size(size_t size) { return (size + 7) & ~7; }

void *arena_alloc(Arena *arena, size_t size) {
#ifdef ENABLE_THREADS
  pthread_mutex_lock(&arena->mutex);
#endif

  size = align_size(size);

  ArenaBlock *block = arena->current;
  if (!block || block->used + size > block->capacity) {
    size_t alloc_size = sizeof(ArenaBlock) +
                        (size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE);
    ArenaBlock *new_block = malloc(alloc_size);
    if (!new_block) {
#ifdef ENABLE_THREADS
      pthread_mutex_unlock(&arena->mutex);
#endif
      return NULL;
    }
    new_block->next = NULL;
    new_block->capacity = alloc_size - sizeof(ArenaBlock);
    new_block->used = 0;
    if (block) {
      while (block->next)
        block = block->next;
      block->next = new_block;
    } else {
      arena->current = new_block;
    }
    block = new_block;
  }

  void *ptr = block->data + block->used;
  block->used += size;

#ifdef ENABLE_THREADS
  pthread_mutex_unlock(&arena->mutex);
#endif
  return ptr;
}

void *arena_alloc_or_panic(Arena *arena, size_t size, jmp_buf env) {
  void *ptr = arena_alloc(arena, size);
  if (!ptr)
    compiler_panic(env, ERR_OOM, "Arena allocation failed");
  return ptr;
}

void arena_free_all(Arena *arena) {
  ArenaBlock *block = arena->current;
  while (block) {
    ArenaBlock *next = block->next;
    free(block);
    block = next;
  }
  arena->current = NULL;
#ifdef ENABLE_THREADS
  pthread_mutex_destroy(&arena->mutex);
#endif
}
