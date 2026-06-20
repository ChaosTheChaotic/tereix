#include <cmocka.h>

#include "arena.h"
#include "sem_types.h"
#include "diag.h"

#ifdef ENABLE_THREADS
#include <stdatomic.h>
#include <unistd.h>
#include "util.h"
extern pthread_mutex_t sem_global_lock;
#endif
extern THREAD_LOCAL Module *sem_current_mod;
extern Module *sem_main_mod;

static void test_sem_valid_assignment(void **state) {
    (void)state;
    Arena arena = {0};
    SemCtx sem = {0};
    sem_init(&sem, &arena);
    
    DiagList diags;
    diaglist_init(&diags, 10);
    sem.diags = &diags;

    const char* code = "i32 main() { mut i32 x = 5; x = 10; ret 0; }";
    AstNode* root = str_to_ast(&arena, code, "test.tx", &diags, true);
    assert_non_null(root);

    Module* mod = new_mod(&arena, "test.tx", "test", root);
    sem_current_mod = mod;
    sem_main_mod = mod;
    map_set(&sem.mod_cache, "test.tx", 7, mod);

    bool sym_res = collect_mod_symbols(&arena, mod, &sem);
    assert_true(sym_res);

    ScopeStack ss;
    scope_stack_init(&ss, &arena);
    resolve_scopes(&arena, mod, &ss, &sem);

    type_check_ast(&arena, mod->ast_root, &sem);

    assert_int_equal(diags.count, 0);

    diaglist_free(&diags);
    sem_deinit(&sem);
    arena_free_all(&arena);
}

static void test_sem_invalid_type_assignment(void **state) {
    (void)state;
    Arena arena = {0};
    SemCtx sem = {0};
    sem_init(&sem, &arena);
    
    DiagList diags;
    diaglist_init(&diags, 10);
    sem.diags = &diags;

    const char* code = "i32 x = \"hello\";";
    AstNode* root = str_to_ast(&arena, code, "test_err.tx", &diags, true);

    Module* mod = new_mod(&arena, "test_err.tx", "test_err", root);
    sem_current_mod = mod;
    map_set(&sem.mod_cache, "test_err.tx", 11, mod);

    collect_mod_symbols(&arena, mod, &sem);
    ScopeStack ss;
    scope_stack_init(&ss, &arena);
    resolve_scopes(&arena, mod, &ss, &sem);
    type_check_ast(&arena, mod->ast_root, &sem);

    assert_true(diags.count > 0);

    diaglist_free(&diags);
    sem_deinit(&sem);
    arena_free_all(&arena);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sem_valid_assignment),
        cmocka_unit_test(test_sem_invalid_type_assignment),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
