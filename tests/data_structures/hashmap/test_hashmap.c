#include <cmocka.h>

#include "hashmap.h"

static void test_map_init(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    
    map_init(&map, &a, 16);

    assert_non_null(map.arena);
    assert_non_null(map.buckets);
    assert_int_equal(map.count, 0);
    assert_int_equal(map.capacity, 16);

    map_free_buckets(&map);
}

static void test_map_set_and_get(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    map_init(&map, &a, 16);

    char *key1 = "name";
    char *val1 = "Alice";
    char *key2 = "role";
    char *val2 = "Admin";

    map_set(&map, key1, strlen(key1), val1);
    map_set(&map, key2, strlen(key2), val2);

    assert_int_equal(map.count, 2);

    char *ret1 = (char *)map_get(&map, key1, strlen(key1));
    char *ret2 = (char *)map_get(&map, key2, strlen(key2));

    assert_string_equal(ret1, "Alice");
    assert_string_equal(ret2, "Admin");

    map_free_buckets(&map);
}

static void test_map_update_key(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    map_init(&map, &a, 16);

    char *key = "score";
    int val1 = 100;
    int val2 = 250;

    map_set(&map, key, strlen(key), &val1);
    assert_int_equal(map.count, 1);

    map_set(&map, key, strlen(key), &val2);
    
    assert_int_equal(map.count, 1); 

    int *ret = (int *)map_get(&map, key, strlen(key));
    assert_int_equal(*ret, 250);

    map_free_buckets(&map);
}

static void test_map_get_missing(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    map_init(&map, &a, 16);

    char *key = "present";
    map_set(&map, key, strlen(key), "data");

    void *ret = map_get(&map, "missing", 7);
    assert_null(ret);

    map_free_buckets(&map);
}

static void test_map_remove_key(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    map_init(&map, &a, 16);

    char *key = "delete_me";
    map_set(&map, key, strlen(key), "temporary");
    assert_int_equal(map.count, 1);

    map_remove(&map, key, strlen(key));
    assert_int_equal(map.count, 0);

    void *ret = map_get(&map, key, strlen(key));
    assert_null(ret);

    map_free_buckets(&map);
}

static void test_map_auto_resize(void **state) {
    (void)state;
    Arena a = {0};
    HashMap map;
    
    map_init(&map, &a, 4); 
    assert_int_equal(map.capacity, 4);

    map_set(&map, "k1", 2, "v1");
    map_set(&map, "k2", 2, "v2");
    map_set(&map, "k3", 2, "v3");
    map_set(&map, "k4", 2, "v4");

    assert_int_equal(map.capacity, 8);
    assert_int_equal(map.count, 4);

    assert_string_equal((char *)map_get(&map, "k1", 2), "v1");
    assert_string_equal((char *)map_get(&map, "k2", 2), "v2");
    assert_string_equal((char *)map_get(&map, "k3", 2), "v3");
    assert_string_equal((char *)map_get(&map, "k4", 2), "v4");

    map_free_buckets(&map);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_map_init),
        cmocka_unit_test(test_map_set_and_get),
        cmocka_unit_test(test_map_update_key),
        cmocka_unit_test(test_map_get_missing),
        cmocka_unit_test(test_map_remove_key),
        cmocka_unit_test(test_map_auto_resize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
