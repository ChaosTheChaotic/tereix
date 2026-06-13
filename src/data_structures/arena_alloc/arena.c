#include "arena.h"

// Align the pointer to the nearest 8 bytes
static size_t align_size(size_t size) { return (size + 7) & ~7; }

ArenaBlock *arena_new_block(size_t size) {
  size_t block_size = size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE;
  // Total size = Block metadata + the data buffer
  ArenaBlock *block = calloc(1, sizeof(ArenaBlock) + block_size);
  block->next = NULL;
  block->capacity = block_size;
  block->used = 0;
  return block;
}

void *arena_alloc(Arena *arena, size_t size) {
  size = align_size(size);

  // If no block exists or current block is full
  if (!arena->current ||
      arena->current->used + size > arena->current->capacity) {
    ArenaBlock *new_block = arena_new_block(size);
    new_block->next = arena->current;
    arena->current = new_block;
  }

  void *ptr = &arena->current->data[arena->current->used];
  arena->current->used += size;
  return ptr;
}

void arena_free_all(Arena *arena) {
  ArenaBlock *curr = arena->current;
  while (curr) {
    ArenaBlock *next = curr->next;
    free(curr);
    curr = next;
  }
  arena->current = NULL;
}
