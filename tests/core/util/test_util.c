#include <unistd.h>
#include <limits.h>
#include <cmocka.h>

#include "arena.h"
#include "util.h"
#include "string_builder.h"

static char *create_temp_file(const char *content) {
    char template[] = "/tmp/test_util_XXXXXX";
    int fd = mkstemp(template);
    if (fd == -1) return NULL;
    if (content) {
        ssize_t written = write(fd, content, strlen(content));
        if (written == -1) {
            close(fd);
            unlink(template);
            return NULL;
        }
    }
    close(fd);
    return strdup(template);
}

static void delete_temp_file(const char *path) {
    if (path) unlink(path);
    free((void*)path);
}

static void test_resolve_alloc_valid_path(void **state) {
    (void)state;
    Arena arena = {0};
    const char *rel = ".";
    const char *abs = resolve_alloc(&arena, rel);
    assert_non_null(abs);
    assert_true(abs[0] == '/');  // absolute path should start with '/'
    arena_free_all(&arena);
}

static void test_resolve_alloc_nonexistent(void **state) {
    (void)state;
    Arena arena = {0};
    const char *bad = "/this/path/does/not/exist/for/sure";
    const char *abs = resolve_alloc(&arena, bad);
    assert_null(abs);
    arena_free_all(&arena);
}

static void test_load_file_success(void **state) {
    (void)state;
    const char *content = "Hello, world!";
    char *path = create_temp_file(content);
    assert_non_null(path);

    const char *loaded = load_file(path);
    assert_non_null(loaded);
    assert_string_equal(loaded, content);

    free((void*)loaded);
    delete_temp_file(path);
}

static void test_load_file_nonexistent(void **state) {
    (void)state;
    const char *bad = "/nonexistent/file/that/is/not/there.txt";
    const char *loaded = load_file(bad);
    assert_null(loaded);
}

static void test_absolute_from_uri_valid(void **state) {
    (void)state;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fail_msg("getcwd failed");
    }
    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", cwd);
    char *abs = absolute_from_uri(uri);
    assert_non_null(abs);
    assert_string_equal(abs, cwd);
    free(abs);
}

static void test_absolute_from_uri_invalid(void **state) {
    (void)state;
    const char *bad_uri = "http://example.com";
    char *abs = absolute_from_uri(bad_uri);
    assert_null(abs);
}

static void test_uri_from_absolute_valid(void **state) {
    (void)state;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fail_msg("getcwd failed");
    }
    char *uri = uri_from_absolute(cwd);
    assert_non_null(uri);
    char expected[PATH_MAX + 8];
    snprintf(expected, sizeof(expected), "file://%s", cwd);
    assert_string_equal(uri, expected);
    free(uri);
}

static void test_uri_from_absolute_null(void **state) {
    (void)state;
    char *uri = uri_from_absolute(NULL);
    assert_null(uri);
}

static void test_uri_from_absolute_relative(void **state) {
    (void)state;
    // relative path should return NULL because it must start with '/'
    char *uri = uri_from_absolute("relative/path");
    assert_null(uri);
}

static void test_extract_mod_name_basic(void **state) {
    (void)state;
    Arena arena = {0};
    const char *path = "/some/dir/mymodule.tx";
    const char *name = extract_mod_name(&arena, path);
    assert_string_equal(name, "mymodule");
    arena_free_all(&arena);
}

static void test_extract_mod_name_no_extension(void **state) {
    (void)state;
    Arena arena = {0};
    const char *path = "/another/path/somemodule";
    const char *name = extract_mod_name(&arena, path);
    assert_string_equal(name, "somemodule");
    arena_free_all(&arena);
}

static void test_extract_mod_name_with_dots(void **state) {
    (void)state;
    Arena arena = {0};
    const char *path = "/dir/foo.bar.baz.tx";
    const char *name = extract_mod_name(&arena, path);
    assert_string_equal(name, "foo.bar.baz");
    arena_free_all(&arena);
}

static void test_extract_mod_name_root_file(void **state) {
    (void)state;
    Arena arena = {0};
    const char *path = "/main.tx";
    const char *name = extract_mod_name(&arena, path);
    assert_string_equal(name, "main");
    arena_free_all(&arena);
}

static void test_check_exists_true(void **state) {
    (void)state;
    char *path = create_temp_file(NULL);
    assert_non_null(path);
    assert_true(check_exists(path));
    delete_temp_file(path);
}

static void test_check_exists_false(void **state) {
    (void)state;
    assert_false(check_exists("/definitely/not/a/valid/file/path"));
}

static void test_file_is_identical_true(void **state) {
    (void)state;
    const char *content = "identical content";
    char *path = create_temp_file(content);
    assert_non_null(path);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, content);
    assert_true(file_is_identical(path, &sb));

    sb_free(&sb);
    delete_temp_file(path);
}

static void test_file_is_identical_false_content(void **state) {
    (void)state;
    const char *content = "original content";
    char *path = create_temp_file(content);
    assert_non_null(path);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "different content");
    assert_false(file_is_identical(path, &sb));

    sb_free(&sb);
    delete_temp_file(path);
}

static void test_file_is_identical_false_length(void **state) {
    (void)state;
    const char *content = "short";
    char *path = create_temp_file(content);
    assert_non_null(path);

    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "much longer content that is not the same");
    assert_false(file_is_identical(path, &sb));

    sb_free(&sb);
    delete_temp_file(path);
}

static void test_file_is_identical_nonexistent(void **state) {
    (void)state;
    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "some text");
    assert_false(file_is_identical("/nonexistent/file", &sb));
    sb_free(&sb);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_resolve_alloc_valid_path),
        cmocka_unit_test(test_resolve_alloc_nonexistent),

        cmocka_unit_test(test_load_file_success),
        cmocka_unit_test(test_load_file_nonexistent),

        cmocka_unit_test(test_absolute_from_uri_valid),
        cmocka_unit_test(test_absolute_from_uri_invalid),
        cmocka_unit_test(test_uri_from_absolute_valid),
        cmocka_unit_test(test_uri_from_absolute_null),
        cmocka_unit_test(test_uri_from_absolute_relative),

        cmocka_unit_test(test_extract_mod_name_basic),
        cmocka_unit_test(test_extract_mod_name_no_extension),
        cmocka_unit_test(test_extract_mod_name_with_dots),
        cmocka_unit_test(test_extract_mod_name_root_file),

        cmocka_unit_test(test_check_exists_true),
        cmocka_unit_test(test_check_exists_false),

        cmocka_unit_test(test_file_is_identical_true),
        cmocka_unit_test(test_file_is_identical_false_content),
        cmocka_unit_test(test_file_is_identical_false_length),
        cmocka_unit_test(test_file_is_identical_nonexistent),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
