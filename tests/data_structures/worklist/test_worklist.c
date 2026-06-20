#include <cmocka.h>

#include "worklist.h"

static void test_wl_push_initial(void **state) {
  (void)state;

  Worklist wl = {0};
  wl_init(&wl);

  wl_push(&wl, "path/to/first");

  assert_int_equal(wl.count, 1);
  assert_int_equal(wl.capacity, 32);
  assert_non_null(wl.paths);
  assert_string_equal(wl.paths[0], "path/to/first");

  wl_destroy(&wl);
}

static void test_wl_pop_lifo(void **state) {
  (void)state;
  Worklist wl = {0};
  wl_init(&wl);

  wl_push(&wl, "first");
  wl_push(&wl, "second");
  wl_push(&wl, "third");

  assert_int_equal(wl.count, 3);

  assert_string_equal(wl_pop(&wl), "third");
  assert_int_equal(wl.count, 2);

  assert_string_equal(wl_pop(&wl), "second");
  assert_int_equal(wl.count, 1);

  assert_string_equal(wl_pop(&wl), "first");
  assert_int_equal(wl.count, 0);

  wl_destroy(&wl);
}

static void test_wl_pop_empty(void **state) {
  (void)state;
  Worklist wl = {0};
  wl_init(&wl);

  wl_done(&wl);
  const char *item = wl_pop(&wl);
  assert_null(item);
  assert_int_equal(wl.count, 0);
  wl_destroy(&wl);
}

static void test_wl_capacity_expansion(void **state) {
  (void)state;
  Worklist wl = {0};
  wl_init(&wl);

  for (int i = 0; i < 32; i++) {
    wl_push(&wl, "dummy_path");
  }
  assert_int_equal(wl.count, 32);
  assert_int_equal(wl.capacity, 32);

  wl_push(&wl, "expanded_path");

  assert_int_equal(wl.count, 33);
  assert_int_equal(wl.capacity, 64);
  assert_string_equal(wl.paths[32], "expanded_path");

  wl_destroy(&wl);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_wl_push_initial),
      cmocka_unit_test(test_wl_pop_lifo),
      cmocka_unit_test(test_wl_pop_empty),
      cmocka_unit_test(test_wl_capacity_expansion),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
