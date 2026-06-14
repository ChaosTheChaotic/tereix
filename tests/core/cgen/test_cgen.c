#include <cmocka.h>

#include "arena.h"
#include "ast_types.h"
#include "hashmap.h"
#include "string_builder.h"

void generate_c_code(AstNode *root, StringBuilder *sb, HashMap *func_map,
                     Arena *arena, bool is_main_mod);
void flatten_sues(AstNode *root, Arena *arena);
void lower_defers(AstNode *root, Arena *arena);

static void test_cgen_basic_func(void **state) {
    (void)state;
    Arena arena = {0};
    DiagList diags;
    diaglist_init(&diags, 10);
    
    const char* code = "i32 add(i32 a, i32 b) { ret a + b; }";
    AstNode* root = str_to_ast(&arena, code, "test.tx", &diags, true);
    assert_non_null(root);

    flatten_sues(root, &arena);
    lower_defers(root, &arena);

    StringBuilder sb;
    sb_init(&sb);
    
    HashMap func_map;
    map_init(&func_map, &arena, 16);
    
    AstNode* func_node = root->as.block.first_stmt;
    assert_non_null(func_node);
    assert_int_equal(func_node->type, AST_FUNC);
    Token fn_name = func_node->as.func_def.fn_name;
    map_set(&func_map, fn_name.start, fn_name.len, func_node);

    generate_c_code(root, &sb, &func_map, &arena, true);

    assert_non_null(strstr(sb.buf, "int32_t add(const int32_t a, const int32_t b)"));
    assert_non_null(strstr(sb.buf, "return (a + b);"));

    sb_free(&sb);
    diaglist_free(&diags);
    arena_free_all(&arena);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cgen_basic_func),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
