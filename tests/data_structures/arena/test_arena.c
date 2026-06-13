#include <cmocka.h>
#include <string.h>

#include "arena.h"

static void test_arena_alloc(void **state) {
  (void)state;

  unsigned int arena_capacity = 1024;
  char *buf;
  Arena arena = {0};
  buf = arena_alloc(&arena, arena_capacity);
  assert_non_null(buf);
  strcpy(buf, "Hello, world!");

  assert_string_equal(buf, "Hello, world!");
  arena_free_all(&arena);
}

int main() {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_arena_alloc),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
