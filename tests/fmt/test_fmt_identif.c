#include <cmocka.h>
#include <stdlib.h>

#include "fmt.h"

char *format_identifier(const char *input, size_t len, FormatStyle style);

static void test_pascal_case(void **state) {
    (void)state;
    char* res;

    res = format_identifier("user_id", 7, FMT_PASCAL_CASE);
    assert_string_equal(res, "UserID");
    free(res);

    res = format_identifier("player_AST_node", 15, FMT_PASCAL_CASE);
    assert_string_equal(res, "PlayerASTNode");
    free(res);

    res = format_identifier("myUserID", 8, FMT_PASCAL_CASE);
    assert_string_equal(res, "MyUserID");
    free(res);
}

static void test_camel_case(void **state) {
    (void)state;
    char* res;

    res = format_identifier("User_ID", 7, FMT_CAMEL_CASE);
    assert_string_equal(res, "userID");
    free(res);

    res = format_identifier("PlayerASTNode", 13, FMT_CAMEL_CASE);
    assert_string_equal(res, "playerASTNode");
    free(res);

    // Testing a single word acronym
    res = format_identifier("ID", 2, FMT_CAMEL_CASE);
    assert_string_equal(res, "id");
    free(res);
}

static void test_snake_case(void **state) {
    (void)state;
    char* res;

    // Functions are snake_case, acronyms become standard lowercase in snake_case
    res = format_identifier("GetUserAST", 10, FMT_SNAKE_CASE);
    assert_string_equal(res, "get_user_ast");
    free(res);

    res = format_identifier("calculate_userID", 16, FMT_SNAKE_CASE);
    assert_string_equal(res, "calculate_user_id");
    free(res);
    
    res = format_identifier("myCamelCaseFunc", 15, FMT_SNAKE_CASE);
    assert_string_equal(res, "my_camel_case_func");
    free(res);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pascal_case),
        cmocka_unit_test(test_camel_case),
        cmocka_unit_test(test_snake_case),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
