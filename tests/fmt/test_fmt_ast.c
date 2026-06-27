#include <cmocka.h>

#include "ast_types.h"
#include "hashmap.h"

AstNode *str_to_ast(Arena *arena, const char *src, const char *file, DiagList *diags, bool is_repl);
void collect_type_names(AstNode *root, HashMap *type_set, Arena *arena);
bool fmt_ast(AstNode *root, FILE *out_fp, HashMap *type_set);

static void assert_format_eq(const char *input, const char *expected) {
    Arena arena = {0};
    DiagList diags;
    diaglist_init(&diags, 1024);

    AstNode *ast = str_to_ast(&arena, input, "test_input.tx", &diags, false);
    assert_non_null(ast);

    HashMap type_set;
    map_init(&type_set, &arena, 128);
    collect_type_names(ast, &type_set, &arena);

    char *buf = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buf, &size);
    assert_non_null(stream);

    bool success = fmt_ast(ast, stream, &type_set);
    fclose(stream);

    assert_true(success);
    assert_non_null(buf);

    assert_string_equal(buf, expected);

    // Cleanup
    free(buf);
    diaglist_free(&diags);
    arena_free_all(&arena);
}

static void test_fmt_variable_declarations(void **state) {
    (void)state;
    const char *input = "threadlocal i32 a = 0 - 1; mut u8 b = 255;";
    const char *expected = 
        "threadlocal i32 a = 0 - 1;\n"
        "mut u8 b = 255;\n";
    
    assert_format_eq(input, expected);
}

static void test_fmt_arrays_and_literals(void **state) {
    (void)state;
    const char *input = "mut u8[3] rarray = [3, 5, { 5; }];";
    const char *expected = 
        "mut u8[3] rarray = [3, 5, {\n"
        "\t5;\n"
        "}];\n";
    
    assert_format_eq(input, expected);
}

static void test_fmt_functions_and_externs(void **state) {
    (void)state;
    const char *input = 
        "extern { str baz(str gleep); i32 putchar(i32 c); }"
        "async inline i32 get_a(bool x) { ret x; }";

    const char *expected = 
        "extern {\n"
        "\tstr baz(str gleep);\n"
        "\ti32 putchar(i32 c);\n"
        "}\n\n"
        "async inline i32 get_a(bool x) {\n"
        "\tret x;\n"
        "}\n";

    assert_format_eq(input, expected);
}

static void test_fmt_structs_unions_enums(void **state) {
    (void)state;
    const char *input = 
        "enum Bee { What = { 1 << 3; }, How }"
        "struct Xexor { mut bool no; i32 size() { ret 3; } }";

    const char *expected = 
        "enum Bee {\n"
        "\tWhat = {\n"
        "\t\t1 << 3;\n"
        "\t},\n"
        "\tHow,\n"
        "}\n\n"
        "struct Xexor {\n"
        "\tmut bool no;\n\n"
        "\ti32 size() {\n"
        "\t\tret 3;\n"
        "\t}\n"
        "}\n\n";

    assert_format_eq(input, expected);
}

static void test_fmt_control_flow(void **state) {
    (void)state;
    const char *input = 
        "void test() { "
        "for (mut u32 i = 0; i < 10; i++) { if (i % 2 == 0) { continue; } else { break; } } "
        "}";

    const char *expected = 
        "void test() {\n"
        "\tfor (mut u32 i = 0; i < 10; i++) {\n"
        "\t\tif (i % 2 == 0) {\n"
        "\t\t\tcontinue;\n"
        "\t\t} else {\n"
        "\t\t\tbreak;\n"
        "\t\t}\n"
        "\t}\n"
        "}\n";

    assert_format_eq(input, expected);
}

static void test_fmt_switch_statement(void **state) {
    (void)state;
    const char *input = 
        "void test(i32 val) { switch (val) { case (1) { val++; } default { val--; } } }";

    const char *expected = 
        "void test(i32 val) {\n"
        "\tswitch (val) {\n"
        "\t\tcase (1) {\n"
        "\t\t\tval++;\n"
        "\t\t}\n"
        "\t\tdefault {\n"
        "\t\t\tval--;\n"
        "\t\t}\n"
        "\t}\n"
        "}\n";

    assert_format_eq(input, expected);
}

static void test_fmt_defers_and_casts(void **state) {
    (void)state;
    const char *input = 
        "struct Xexor { bool hi; i32 hello; } void test() { mut i64 bee = (i64)b; defer { b + 3; } defer b + 3; putchar((i32)sizeof(Xexor)); }";

    const char *expected = 
				"struct Xexor {\n"
				"\tbool hi;\n"
				"\ti32 hello;\n"
				"}\n\n"
        "void test() {\n"
        "\tmut i64 bee = (i64)b;\n"
        "\tdefer {\n"
        "\t\tb + 3;\n"
        "\t}\n"
        "\tdefer b + 3;\n"
        "\tputchar((i32)sizeof(Xexor));\n"
        "}\n";

    assert_format_eq(input, expected);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fmt_variable_declarations),
        cmocka_unit_test(test_fmt_arrays_and_literals),
        cmocka_unit_test(test_fmt_functions_and_externs),
        cmocka_unit_test(test_fmt_structs_unions_enums),
        cmocka_unit_test(test_fmt_control_flow),
        cmocka_unit_test(test_fmt_switch_statement),
        cmocka_unit_test(test_fmt_defers_and_casts),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
