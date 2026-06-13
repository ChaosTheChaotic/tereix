#include <cmocka.h>
#include <string.h>

#include "hashutils.h"

static void test_hash_string_empty_and_null(void **state) {
    (void)state;

    assert_int_equal(hash_string(NULL, 10), 0);
    assert_int_equal(hash_string("", 0), 2166136261u);
}

static void test_hash_string_values(void **state) {
    (void)state;

    char *str1 = "a";
    uint32_t hash1 = hash_string(str1, strlen(str1));
    assert_int_equal(hash1, 3826002220u);

    char *str2 = "xyz";
    uint32_t hash2 = hash_string(str2, strlen(str2));
    assert_int_equal(hash2, 3298945248u);
}

static void test_hash_string_case_sensitivity(void **state) {
    (void)state;

    uint32_t lower_hash = hash_string("test", 4);
    uint32_t upper_hash = hash_string("TEST", 4);

    assert_int_not_equal(lower_hash, upper_hash);
}

static void test_combine_hash_values(void **state) {
    (void)state;

    uint32_t current_hash = 2166136261u;
    uint32_t new_value = 0x12345678;

    uint32_t combined = combine_hash(current_hash, new_value);
    assert_int_equal(combined, 2741278597u);
}

static void test_combine_hash_order_sensitivity(void **state) {
    (void)state;

    uint32_t base_hash = 2166136261u;

    uint32_t res1 = combine_hash(base_hash, 42);
    uint32_t res2 = combine_hash(base_hash, 99);
    assert_int_not_equal(res1, res2);

    uint32_t order_ab = combine_hash(combine_hash(base_hash, 100), 200);
    uint32_t order_ba = combine_hash(combine_hash(base_hash, 200), 100);
    assert_int_not_equal(order_ab, order_ba);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_hash_string_empty_and_null),
        cmocka_unit_test(test_hash_string_values),
        cmocka_unit_test(test_hash_string_case_sensitivity),
        cmocka_unit_test(test_combine_hash_values),
        cmocka_unit_test(test_combine_hash_order_sensitivity),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
