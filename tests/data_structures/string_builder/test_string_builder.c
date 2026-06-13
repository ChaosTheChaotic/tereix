#include <cmocka.h>

#include "string_builder.h"

static void test_sb_init(void **state) {
    (void)state;
    StringBuilder sb;

    sb_init(&sb);

    assert_int_equal(sb.cap, 2048);
    assert_int_equal(sb.len, 0);
    assert_non_null(sb.buf);
    
    assert_string_equal(sb.buf, "");
    assert_int_equal(sb.buf[0], '\0');

    sb_free(&sb);
}

static void test_sb_append(void **state) {
    (void)state;
    StringBuilder sb;
    sb_init(&sb);

    sb_append(&sb, "Hello");
    assert_int_equal(sb.len, 5);
    assert_string_equal(sb.buf, "Hello");

    sb_append(&sb, " World");
    assert_int_equal(sb.len, 11);
    assert_string_equal(sb.buf, "Hello World");

    assert_int_equal(sb.buf[11], '\0');

    sb_free(&sb);
}

static void test_sb_append_len(void **state) {
    (void)state;
    StringBuilder sb;
    sb_init(&sb);

    char *long_str = "Test_Extra_Data";
    sb_append_len(&sb, long_str, 4);

    assert_int_equal(sb.len, 4);
    assert_string_equal(sb.buf, "Test");
    assert_int_equal(sb.buf[4], '\0');

    sb_free(&sb);
}

static void test_sb_auto_resize(void **state) {
    (void)state;
    StringBuilder sb;
    sb_init(&sb);

    size_t large_size = 2500;
    char *large_str = malloc(large_size + 1);
    memset(large_str, 'A', large_size);
    large_str[large_size] = '\0';

    sb_append(&sb, large_str);

    assert_int_equal(sb.len, large_size);
    
    assert_int_equal(sb.cap, 5002);
    
    assert_int_equal(sb.buf[0], 'A');
    assert_int_equal(sb.buf[large_size - 1], 'A');
    assert_int_equal(sb.buf[large_size], '\0');

    free(large_str);
    sb_free(&sb);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sb_init),
        cmocka_unit_test(test_sb_append),
        cmocka_unit_test(test_sb_append_len),
        cmocka_unit_test(test_sb_auto_resize),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
