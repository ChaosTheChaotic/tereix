#include <cmocka.h>

#include "arena.h"
#include "ast_types.h"
#include "diag.h"

static AstNode* parse_code(Arena* arena, DiagList* diags, const char* code) {
    return str_to_ast(arena, code, "test.tx", diags, false);
}

static void test_parse_var_decl(void **state) {
    (void)state;
    Arena arena = {0};
    DiagList diags;
    diaglist_init(&diags, 10);
    
    const char* code = "i32 x = 5;";
    AstNode* root = parse_code(&arena, &diags, code);
    
    assert_non_null(root);
    assert_int_equal(root->type, AST_PROGRAM);
    
    AstNode* stmt = root->as.block.first_stmt;
    assert_non_null(stmt);
    assert_int_equal(stmt->type, AST_VAR_DECL);
    
    // Check variable identifier
    assert_int_equal(stmt->as.var_decl.id.len, 1);
    assert_memory_equal(stmt->as.var_decl.id.start, "x", 1);
    
    // Check initialization expression
    AstNode* init = stmt->as.var_decl.init;
    assert_non_null(init);
    assert_int_equal(init->type, AST_NUM_LIT);
    assert_memory_equal(init->as.num_lit.val.start, "5", 1);
    
    assert_int_equal(diags.count, 0);

    diaglist_free(&diags);
    arena_free_all(&arena);
}

static void test_parse_struct_def(void **state) {
    (void)state;
    Arena arena = {0};
    DiagList diags;
    diaglist_init(&diags, 10);
    
    const char* code = "struct Vector2 { i32 x; i32 y; }";
    AstNode* root = parse_code(&arena, &diags, code);
    
    assert_non_null(root);
    AstNode* stmt = root->as.block.first_stmt;
    
    assert_non_null(stmt);
    assert_int_equal(stmt->type, AST_STRUCT);
    assert_memory_equal(stmt->as.struct_def.structn.start, "Vector2", 7);
    
    // Check fields
    AstNode* field1 = stmt->as.struct_def.contents;
    assert_non_null(field1);
    assert_int_equal(field1->type, AST_VAR_DECL);
    assert_memory_equal(field1->as.var_decl.id.start, "x", 1);
    
    AstNode* field2 = field1->next;
    assert_non_null(field2);
    assert_int_equal(field2->type, AST_VAR_DECL);
    assert_memory_equal(field2->as.var_decl.id.start, "y", 1);

    assert_int_equal(diags.count, 0);

    diaglist_free(&diags);
    arena_free_all(&arena);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_var_decl),
        cmocka_unit_test(test_parse_struct_def),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
