#ifndef ARENA_H
#define ARENA_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define ARENA_CHUNK_SIZE (1024 * 1024)

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t capacity;
  size_t used;
  uint8_t data[];
} ArenaBlock;

typedef struct {
  ArenaBlock *current;
} Arena;

void *arena_alloc(Arena *arena, size_t size);
void arena_free_all(Arena *arena);

#endif
