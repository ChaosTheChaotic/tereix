#include <cmocka.h>

#include "arena.h"
#include "ast_serde.h"
#include "diag.h"

static void test_ast_serde_roundtrip(void **state) {
    (void)state;
    Arena write_arena = {0};
    Arena read_arena = {0};
    DiagList diags;
    diaglist_init(&diags, 10);
    
    const char* code = "i32 my_cached_var = 42;";
    AstNode* root = str_to_ast(&write_arena, code, "test.tx", &diags, false);
    assert_non_null(root);

    const char* cache_file = "test_serde.cache";
    uint64_t dummy_hash = 999999;
    
    // Pass a dummy FILE ptr internally by just creating it in the local dir
    cache_write_ast(cache_file, root, code, dummy_hash);

    // Passing sizeof(uint64_t) skips the header hash byte your caching logic uses
    AstNode* read_root = cache_read_ast(&read_arena, cache_file, code, sizeof(uint64_t));
    
    assert_non_null(read_root);
    assert_int_equal(read_root->type, AST_PROGRAM);
    
    AstNode* stmt = read_root->as.block.first_stmt;
    assert_non_null(stmt);
    assert_int_equal(stmt->type, AST_VAR_DECL);
    
    // Confirm exact tokens are preserved
    assert_int_equal(stmt->as.var_decl.id.len, 13);
    assert_memory_equal(stmt->as.var_decl.id.start, "my_cached_var", 13);
    
    AstNode* init = stmt->as.var_decl.init;
    assert_non_null(init);
    assert_int_equal(init->type, AST_NUM_LIT);
    assert_memory_equal(init->as.num_lit.val.start, "42", 2);

    // Cleanup
    remove(cache_file);
    diaglist_free(&diags);
    arena_free_all(&write_arena);
    arena_free_all(&read_arena);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ast_serde_roundtrip),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
