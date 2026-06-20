#include "ast_serde.h"
#include "c_gen_types.h"
#include "cli.h"
#include "fmt.h"
#include "hashutils.h"
#include "lsp.h"
#include "tereix_version.h"
#include "util.h"
#include "worklist.h"
#include <sys/stat.h>

#ifdef ENABLE_THREADS
#include "thread_pool.h"
#include <stdatomic.h>
#include <unistd.h>
extern pthread_mutex_t sem_global_lock;
#endif
extern THREAD_LOCAL Module *sem_current_mod;
extern Module *sem_main_mod;

typedef struct ParseTaskData {
  const char *path;
  SemCtx *sem;
  Worklist *worklist;
  Arena *arena;
} ParseTaskData;

void extract_dependencies(AstNode *root, void (*callback)(Token callee_name)) {
  if (!root)
    return;

  size_t stack_cap = 1024;
  AstNode **stack = malloc(sizeof(AstNode *) * stack_cap);
  size_t top = 0;

  stack[top++] = root;

  while (top > 0) {
    AstNode *node = stack[--top];
    if (!node)
      continue;

    // Check for explicit Function Calls or Method calls
    if (node->type == AST_FUNC_CALL && node->as.func_call.caller) {
      if (node->as.func_call.caller->type == AST_IDENTIF) {
        callback(node->as.func_call.caller->as.identif.val);
      } else if (node->as.func_call.caller->type == AST_MEMBER) {
        callback(node->as.func_call.caller->as.member.name);
      }
    }

    // Push sibling to stack first
    if (node->next) {
      if (top >= stack_cap - 2) {
        stack_cap *= 2;
        stack = realloc(stack, sizeof(AstNode *) * stack_cap);
      }
      stack[top++] = node->next;
    }

#define PUSH_NODE(n)                                                           \
  do {                                                                         \
    if (n) {                                                                   \
      if (top >= stack_cap - 2) {                                              \
        stack_cap *= 2;                                                        \
        stack = realloc(stack, sizeof(AstNode *) * stack_cap);                 \
      }                                                                        \
      stack[top++] = (n);                                                      \
    }                                                                          \
  } while (0)

    // Queue up the children for subsequent loops
    switch (node->type) {
    case AST_BINOP:
      PUSH_NODE(node->as.binop.left);
      PUSH_NODE(node->as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PUSH_NODE(node->as.unop.operand);
      break;
    case AST_IF:
      PUSH_NODE(node->as.if_check.check);
      PUSH_NODE(node->as.if_check.action);
      PUSH_NODE(node->as.if_check.elseAct);
      break;
    case AST_BLOCK:
    case AST_PROGRAM:
      PUSH_NODE(node->as.block.first_stmt);
      break;
    case AST_FUNC:
      PUSH_NODE(node->as.func_def.block);
      break;
    case AST_RET:
      PUSH_NODE(node->as.ret_stmt.expr);
      break;
    case AST_VAR_DECL:
      PUSH_NODE(node->as.var_decl.init);
      break;
    case AST_ARRAY_LIT:
      PUSH_NODE(node->as.array_lit.elements);
      break;
    case AST_STRUCT:
      PUSH_NODE(node->as.struct_def.contents);
      break;
    case AST_UNION:
      PUSH_NODE(node->as.union_def.contents);
      break;
    case AST_ENUM:
      PUSH_NODE(node->as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      PUSH_NODE(node->as.enum_member.val);
      break;
    case AST_DEFER:
      PUSH_NODE(node->as.defer_stmt.contents);
      break;
    case AST_FOR:
      PUSH_NODE(node->as.for_loop.init);
      PUSH_NODE(node->as.for_loop.check);
      PUSH_NODE(node->as.for_loop.inc);
      PUSH_NODE(node->as.for_loop.action);
      break;
    case AST_WHILE:
      PUSH_NODE(node->as.while_loop.check);
      PUSH_NODE(node->as.while_loop.action);
      break;
    case AST_FUNC_CALL:
      PUSH_NODE(node->as.func_call.caller);
      PUSH_NODE(node->as.func_call.args);
      break;
    case AST_INDEX:
      PUSH_NODE(node->as.index.base);
      PUSH_NODE(node->as.index.index);
      break;
    case AST_MEMBER:
      PUSH_NODE(node->as.member.base);
      break;
    case AST_SWITCH:
      PUSH_NODE(node->as.switch_stmt.check);
      PUSH_NODE(node->as.switch_stmt.cases);
      PUSH_NODE(node->as.switch_stmt.default_case);
      break;
    case AST_CASE:
      PUSH_NODE(node->as.case_stmt.val);
      PUSH_NODE(node->as.case_stmt.action);
      break;
    case AST_EXTERN:
      PUSH_NODE(node->as.extern_block.contents);
      break;
    case AST_CAST:
      PUSH_NODE(node->as.cast.op);
      break;
    case AST_SIZEOF:
      PUSH_NODE(node->as.sizeof_expr.target_expr);
      break;
    default:
      break;
    }

#undef PUSH_NODE
  }

  free(stack);
}

void sync_dirty_flags_to_ast(Module *mod) {
  if (!mod->ast_root || mod->ast_root->type != AST_PROGRAM)
    return;

  AstNode *stmt = mod->ast_root->as.block.first_stmt;
  while (stmt) {
    Token name = {0};

    if (stmt->type == AST_FUNC)
      name = stmt->as.func_def.fn_name;
    else if (stmt->type == AST_STRUCT)
      name = stmt->as.struct_def.structn;
    else if (stmt->type == AST_UNION)
      name = stmt->as.union_def.unionn;
    else if (stmt->type == AST_ENUM)
      name = stmt->as.enum_def.enumn;
    else if (stmt->type == AST_VAR_DECL)
      name = stmt->as.var_decl.id;

    if (name.start) {
      stmt->is_dirty = false;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        // Match AST top level nodes back to their cached metadata
        if (m->name.len == name.len &&
            memcmp(m->name.start, name.start, name.len) == 0) {
          stmt->is_dirty = m->is_dirty;
          break;
        }
      }
    } else {
      stmt->is_dirty = true;
    }
    stmt = stmt->next;
  }
}

typedef struct DepList {
  DeclMetadata *decl;
  struct DepList *next;
} DepList;

void propagate_global_invalidation(SemCtx *sem) {
  size_t total_decls = 0;
  Arena temp_arena = {0}; // Temporary arena for the reverse graph
  HashMap reverse_deps;
  map_init(&reverse_deps, &temp_arena, 4096);

  // Build the Reverse Dependency Graph
  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        total_decls++;

        for (DepNode *dep = m->calls_to; dep; dep = dep->next) {
          DepList *link = arena_alloc(&temp_arena, sizeof(DepList));
          link->decl = m;
          link->next = map_get(&reverse_deps, dep->name.start, dep->name.len);
          map_set(&reverse_deps, dep->name.start, dep->name.len, link);
        }

        for (DepNode *dep = m->uses_types; dep; dep = dep->next) {
          DepList *link = arena_alloc(&temp_arena, sizeof(DepList));
          link->decl = m;
          link->next = map_get(&reverse_deps, dep->name.start, dep->name.len);
          map_set(&reverse_deps, dep->name.start, dep->name.len, link);
        }
      }
      entry = entry->next;
    }
  }

  if (total_decls == 0)
    return;

  DeclMetadata **worklist = malloc(sizeof(DeclMetadata *) * total_decls);
  size_t wl_top = 0;

  // Queue initially dirty items
  for (size_t i = 0; i < sem->mod_cache.capacity; i++) {
    HashEntry *entry = sem->mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      for (DeclMetadata *m = mod->meta; m; m = m->next) {
        if (m->is_dirty)
          worklist[wl_top++] = m;
      }
      entry = entry->next;
    }
  }

  // Propagate using the reverse graph
  while (wl_top > 0) {
    DeclMetadata *dirty_item = worklist[--wl_top];

    // Instantly get all declarations that depend on this dirty item
    DepList *dependents =
        map_get(&reverse_deps, dirty_item->name.start, dirty_item->name.len);

    while (dependents) {
      DeclMetadata *dependent_decl = dependents->decl;
      if (!dependent_decl->is_dirty) {
        dependent_decl->is_dirty = true;
        worklist[wl_top++] = dependent_decl;
      }
      dependents = dependents->next;
    }
  }

  free(worklist);
  arena_free_all(&temp_arena);
}

#ifdef ENABLE_THREADS
typedef struct {
  SemCtx *sem;
  Worklist *worklist;
  Arena *global_arena;
  pthread_mutex_t *global_arena_mutex;
  volatile int *error_occurred;
  atomic_size_t *modules_in_flight;
  uint64_t env_hash;
} WorkerData;

bool worker_loop(void *arg) {
  WorkerData *data = (WorkerData *)arg;
  SemCtx *sem = data->sem;
  Worklist *wl = data->worklist;
  Arena *global = data->global_arena;
  pthread_mutex_t *arena_mutex = data->global_arena_mutex;
  volatile int *error_flag = data->error_occurred;
  atomic_size_t *in_flight = data->modules_in_flight;
  uint64_t env_hash = data->env_hash;

  while (1) {
    const char *rel_path = wl_pop(wl);
    if (!rel_path)
      break; // worklist done

    const char *abs_path = resolve_alloc(global, rel_path);
    if (!abs_path) {
      fprintf(stderr, "Failed to resolve path: %s\n", rel_path);
      *error_flag = 1;
      atomic_fetch_sub(in_flight, 1);
      continue;
    }

    pthread_mutex_lock(&sem->mutex);
    Module *existing = map_get(&sem->mod_cache, abs_path, strlen(abs_path));
    if (existing) {
      pthread_mutex_unlock(&sem->mutex);
      atomic_fetch_sub(in_flight, 1);
      continue;
    }
    map_set(&sem->mod_cache, abs_path, strlen(abs_path),
            (Module *)1); // (Module*)1 acts as in progress
    pthread_mutex_unlock(&sem->mutex);

    // Create per module arena
    pthread_mutex_lock(arena_mutex);
    Arena *mod_arena = arena_alloc(global, sizeof(Arena));
    *mod_arena = (Arena){0};
    pthread_mutex_unlock(arena_mutex);

    const char *content = load_file_into_arena(mod_arena, abs_path);
    if (!content) {
      fprintf(stderr, "Failed to load file: %s\n", abs_path);
      *error_flag = 1;
      atomic_fetch_sub(in_flight, 1);
      continue;
    }

    uint64_t file_hash = hash_string(content, strlen(content));
    uint64_t curr_hash = combine_hash(file_hash, env_hash);

    char cache_file[512], meta_file[512];
    uint64_t path_hash = hash_string(abs_path, strlen(abs_path));
    snprintf(cache_file, sizeof(cache_file), ".tx_cache/%lu.cache", path_hash);
    snprintf(meta_file, sizeof(meta_file), ".tx_cache/%lu.meta", path_hash);

    uint64_t cached_content_hash = 0;
    FILE *fp = fopen(cache_file, "rb");
    if (fp) {
      if (fread(&cached_content_hash, sizeof(uint64_t), 1, fp) != 1)
        cached_content_hash = 0;
      fclose(fp);
    }

    AstNode *ast = NULL;
    bool cache_loaded = false;
    DeclMetadata *meta = NULL;

    if (cached_content_hash == curr_hash) {
      ast = cache_read_ast(mod_arena, cache_file, content, sizeof(uint64_t));
      if (ast) {
        cache_loaded = true;
        meta = cache_read_decl_meta(mod_arena, meta_file, content);
        if (meta) {
          for (DeclMetadata *m = meta; m; m = m->next)
            m->is_dirty = false;
        }
      }
    }

    if (!cache_loaded) {
      DiagList diags;
      diaglist_init(&diags, 1024);
      ast = str_to_ast(mod_arena, content, abs_path, &diags, false);
      if (!ast) {
        pthread_mutex_lock(&sem->mutex);
        for (size_t i = 0; i < diags.count; i++) {
          Token tok = {
              .start = NULL,
              .len = 0,
              .line = diags.items[i].start_line,
              .col = diags.items[i].start_char};
          sem_report(sem, DIAG_ERROR, tok, "%s", diags.items[i].message);
        }
        pthread_mutex_unlock(&sem->mutex);
        diaglist_free(&diags);
        *error_flag = 1;
        atomic_fetch_sub(in_flight, 1);
        continue;
      }
      diaglist_free(&diags);

      meta = analyze_module_declarations(mod_arena, ast);
      DeclMetadata *old_meta =
          cache_read_decl_meta(mod_arena, meta_file, content);
      propagate_declaration_invalidation(old_meta, meta);

      // Sync dirty flags to AST nodes
      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        Token name = {0};
        if (stmt->type == AST_FUNC)
          name = stmt->as.func_def.fn_name;
        else if (stmt->type == AST_STRUCT)
          name = stmt->as.struct_def.structn;
        else if (stmt->type == AST_UNION)
          name = stmt->as.union_def.unionn;
        else if (stmt->type == AST_ENUM)
          name = stmt->as.enum_def.enumn;
        else if (stmt->type == AST_VAR_DECL)
          name = stmt->as.var_decl.id;

        stmt->is_dirty = true;
        if (name.start) {
          for (DeclMetadata *m = meta; m; m = m->next) {
            if (m->name.len == name.len &&
                memcmp(m->name.start, name.start, name.len) == 0) {
              stmt->is_dirty = m->is_dirty;
              break;
            }
          }
        }
        stmt = stmt->next;
      }

      // Splice cached typed nodes
      AstNode *old_ast =
          cache_read_ast(mod_arena, cache_file, content, sizeof(uint64_t));
      AstNode **ptr = &ast->as.block.first_stmt;
      while (*ptr) {
        AstNode *curr = *ptr;
        if (!curr->is_dirty &&
            (curr->type == AST_FUNC || curr->type == AST_VAR_DECL ||
             curr->type == AST_STRUCT || curr->type == AST_UNION ||
             curr->type == AST_ENUM)) {
          bool spliced = false;
          if (old_ast) {
            AstNode *old_stmt = old_ast->as.block.first_stmt;
            while (old_stmt) {
              if (old_stmt->node_hash == curr->node_hash &&
                  old_stmt->type == curr->type) {
                old_stmt->next = curr->next;
                old_stmt->is_dirty = false;
                *ptr = old_stmt;
                spliced = true;
                break;
              }
              old_stmt = old_stmt->next;
            }
          }
          if (!spliced) {
            curr->is_dirty = true;
            Token name = {0};
            if (curr->type == AST_FUNC)
              name = curr->as.func_def.fn_name;
            else if (curr->type == AST_STRUCT)
              name = curr->as.struct_def.structn;
            else if (curr->type == AST_UNION)
              name = curr->as.union_def.unionn;
            else if (curr->type == AST_ENUM)
              name = curr->as.enum_def.enumn;
            else if (curr->type == AST_VAR_DECL)
              name = curr->as.var_decl.id;
            if (name.start) {
              for (DeclMetadata *m = meta; m; m = m->next) {
                if (m->name.len == name.len &&
                    memcmp(m->name.start, name.start, name.len) == 0) {
                  m->is_dirty = true;
                  break;
                }
              }
            }
          }
        }
        ptr = &(*ptr)->next;
      }
      cache_write_decl_meta(meta_file, meta, content);
    }

    const char *mod_name = extract_mod_name(mod_arena, abs_path);
    Module *mod = new_mod(mod_arena, abs_path, mod_name, ast);
    mod->content_hash = curr_hash;
    mod->meta = meta;
    mod->content = content;
    mod->needs_cache_write = !cache_loaded;

    pthread_mutex_lock(&sem->mutex);
    map_set(&sem->mod_cache, abs_path, strlen(abs_path), mod);
    pthread_mutex_unlock(&sem->mutex);

    // Push use statement dependencies
    AstNode *stmt = ast->as.block.first_stmt;
    while (stmt) {
      if (stmt->type == AST_USE) {
        size_t path_len = stmt->as.use_stmt.path.len;
        if (path_len > 2) {
          char *clean_rel = arena_alloc(mod_arena, path_len - 1);
          strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
          clean_rel[path_len - 2] = '\0';
          const char *normalized =
              normalize_module_path(data->global_arena, clean_rel);
          atomic_fetch_add(in_flight, 1);
          wl_push(wl, normalized);
        }
      }
      stmt = stmt->next;
    }

    atomic_fetch_sub(in_flight, 1);
  }
  return true;
}
#endif // ENABLE_THREADS

void compile_project(const CompileOptions *restrict opts) {
  ensure_cache_dir();
  Arena arena = {0};
  SemCtx sem = {0};
  sem_init(&sem, &arena);

  char env_sig[2048] = {0};
  int sig_len = snprintf(env_sig, sizeof(env_sig), "%s", TEREIX_BUILD_HASH);
  uint64_t env_hash = hash_string(env_sig, sig_len);

  sem_current_mod = NULL;
  sem_main_mod = NULL;

  Worklist pending = {0};
  wl_init(&pending);

  // Push the starting module
  wl_push(&pending, opts->input_file);

#ifdef ENABLE_THREADS
  int num_threads = opts->thread_count;
  if (num_threads <= 0) {
    num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads < 1)
      num_threads = 1;
    if (num_threads > 64)
      num_threads = 64;
  }

  ThreadPool *pool = tp_create(num_threads);
  pthread_mutex_t arena_mutex;
  pthread_mutex_init(&arena_mutex, NULL);
  volatile int error_occurred = 0;
  atomic_size_t modules_in_flight = 1; // Initial path pushed

  WorkerData wdata = {.sem = &sem,
                      .worklist = &pending,
                      .global_arena = &arena,
                      .global_arena_mutex = &arena_mutex,
                      .error_occurred = &error_occurred,
                      .modules_in_flight = &modules_in_flight,
                      .env_hash = env_hash};

  // Submit one task per thread each task will loop until worklist is done
  for (int i = 0; i < num_threads; i++) {
    tp_submit(pool, (TaskFunc)worker_loop, &wdata);
  }

  // Wait until all modules have been processed
  while (atomic_load(&modules_in_flight) > 0) {
    // Yield to avoid busy‑waiting
    sched_yield();
  }
  wl_done(&pending); // signal workers that no more items will arrive

  tp_wait(pool);
  tp_destroy(pool);
  pthread_mutex_destroy(&arena_mutex);

  if (error_occurred) {
    fprintf(stderr, "Errors occurred during parsing, exiting.\n");
    exit(1);
  }
#else
  const char *current_path;
	unsigned int single_thread_error = 0;
  while ((current_path = wl_pop(&pending)) != NULL) {
    const char *abs_path = resolve_alloc(&arena, current_path);
    if (!abs_path || map_get(&sem.mod_cache, abs_path, strlen(abs_path)))
      continue;

    const char *content = load_file(abs_path);
    uint64_t file_hash = hash_string(content, strlen(content));
    uint64_t curr_hash = combine_hash(file_hash, env_hash);
    uint64_t path_hash = hash_string(abs_path, strlen(abs_path));

    char cache_file[512], meta_file[512];
    snprintf(cache_file, sizeof(cache_file), ".tx_cache/%lu.cache", path_hash);
    snprintf(meta_file, sizeof(meta_file), ".tx_cache/%lu.meta", path_hash);

    AstNode *ast = NULL;
    bool cache_loaded = false;
    DeclMetadata *meta = NULL;

    uint64_t cached_content_hash = 0;
    FILE *fp = fopen(cache_file, "rb");
    if (fp) {
      if (fread(&cached_content_hash, sizeof(uint64_t), 1, fp) != 1)
        cached_content_hash = 0;
      fclose(fp);
    }

    if (cached_content_hash == curr_hash) {
      ast = cache_read_ast(&arena, cache_file, content, sizeof(uint64_t));
      if (ast) {
        printf("Cache hit for %s\n", abs_path);
        cache_loaded = true;
        meta = cache_read_decl_meta(&arena, meta_file, content);
        if (meta) {
          for (DeclMetadata *m = meta; m; m = m->next)
            m->is_dirty = false;
        }
      }
    }

    if (!cache_loaded) {
      printf("Compiling %s\n", abs_path);
      DiagList diags;
      diaglist_init(&diags, 1024);
      ast = str_to_ast(&arena, content, abs_path, &diags, false);
      if (!ast) {
        for (size_t i = 0; i < diags.count; i++) {
          Token tok = {.start = NULL,
                       .len = 0,
                       .line = diags.items[i].start_line,
                       .col = diags.items[i].start_char};
          sem_report(&sem, DIAG_ERROR, tok, "%s", diags.items[i].message);
        }
        diaglist_free(&diags);
        single_thread_error = 1;
        continue;
      }
      diaglist_free(&diags);

      meta = analyze_module_declarations(&arena, ast);
      DeclMetadata *old_meta = cache_read_decl_meta(&arena, meta_file, content);
      propagate_declaration_invalidation(old_meta, meta);

      AstNode *stmt = ast->as.block.first_stmt;
      while (stmt) {
        Token name = {0};
        if (stmt->type == AST_FUNC)
          name = stmt->as.func_def.fn_name;
        else if (stmt->type == AST_STRUCT)
          name = stmt->as.struct_def.structn;
        else if (stmt->type == AST_UNION)
          name = stmt->as.union_def.unionn;
        else if (stmt->type == AST_ENUM)
          name = stmt->as.enum_def.enumn;
        else if (stmt->type == AST_VAR_DECL)
          name = stmt->as.var_decl.id;
        stmt->is_dirty = true;
        if (name.start) {
          for (DeclMetadata *m = meta; m; m = m->next) {
            if (m->name.len == name.len &&
                memcmp(m->name.start, name.start, name.len) == 0) {
              stmt->is_dirty = m->is_dirty;
              break;
            }
          }
        }
        stmt = stmt->next;
      }

      AstNode *old_ast =
          cache_read_ast(&arena, cache_file, content, sizeof(uint64_t));
      AstNode **ptr = &ast->as.block.first_stmt;
      while (*ptr) {
        AstNode *curr = *ptr;
        if (!curr->is_dirty &&
            (curr->type == AST_FUNC || curr->type == AST_VAR_DECL ||
             curr->type == AST_STRUCT || curr->type == AST_UNION ||
             curr->type == AST_ENUM)) {
          bool spliced = false;
          if (old_ast) {
            AstNode *old_stmt = old_ast->as.block.first_stmt;
            while (old_stmt) {
              if (old_stmt->node_hash == curr->node_hash &&
                  old_stmt->type == curr->type) {
                old_stmt->next = curr->next;
                old_stmt->is_dirty = false;
                *ptr = old_stmt;
                spliced = true;
                break;
              }
              old_stmt = old_stmt->next;
            }
          }
          if (!spliced) {
            curr->is_dirty = true;
            Token name = {0};
            if (curr->type == AST_FUNC)
              name = curr->as.func_def.fn_name;
            else if (curr->type == AST_STRUCT)
              name = curr->as.struct_def.structn;
            else if (curr->type == AST_UNION)
              name = curr->as.union_def.unionn;
            else if (curr->type == AST_ENUM)
              name = curr->as.enum_def.enumn;
            else if (curr->type == AST_VAR_DECL)
              name = curr->as.var_decl.id;
            if (name.start) {
              for (DeclMetadata *m = meta; m; m = m->next) {
                if (m->name.len == name.len &&
                    memcmp(m->name.start, name.start, name.len) == 0) {
                  m->is_dirty = true;
                  break;
                }
              }
            }
          }
        }
        ptr = &(*ptr)->next;
      }
      cache_write_decl_meta(meta_file, meta, content);
    }

    const char *mod_name = extract_mod_name(&arena, abs_path);
    Module *mod = new_mod(&arena, abs_path, mod_name, ast);
    mod->content_hash = curr_hash;
    mod->meta = meta;
    mod->content = content;
    mod->needs_cache_write = !cache_loaded;

    if (sem_main_mod == NULL)
      sem_main_mod = mod;

    if (opts->print_ast) {
      printf("Module: %s\n", mod_name);
      print_ast(ast);
    }

    map_set(&sem.mod_cache, abs_path, strlen(abs_path), mod);

    AstNode *stmt = ast->as.block.first_stmt;
    while (stmt) {
      if (stmt->type == AST_USE) {
        size_t path_len = stmt->as.use_stmt.path.len;
        if (path_len > 2) {
          char *clean_rel = arena_alloc(&arena, path_len - 1);
          strncpy(clean_rel, stmt->as.use_stmt.path.start + 1, path_len - 2);
          clean_rel[path_len - 2] = '\0';
          const char *normalized = normalize_module_path(&arena, clean_rel);
          wl_push(&pending, normalized);
        }
      }
      stmt = stmt->next;
    }
  }
  if (single_thread_error) {
    fprintf(stderr, "Errors occurred during parsing, exiting.\n");
    exit(1);
  }
  printf("AST Construction complete.\n");
#endif // ENABLE_THREADS

  propagate_global_invalidation(&sem);

  bool requires_rebuild = false;
  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sync_dirty_flags_to_ast(mod);
      AstNode *stmt = mod->ast_root->as.block.first_stmt;
      while (stmt) {
        if (stmt->is_dirty)
          requires_rebuild = true;
        stmt = stmt->next;
      }
      entry = entry->next;
    }
  }

  if (!requires_rebuild) {
    printf("Project is up to date.\n");
    if (pending.paths)
      free((void *)pending.paths);
    sem_deinit(&sem);
    arena_free_all(&arena);
    return;
  }

  printf("Global dependency propagation complete.\n");

  resolve_imports(&arena, &sem);
  printf("Import graph resolved.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sem_current_mod = mod;
      collect_mod_symbols(&arena, mod, &sem);
      entry = entry->next;
    }
  }
  printf("Symbol collection complete.\n");

  ScopeStack ss;
  scope_stack_init(&ss, &arena);

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      ss.count = 0;
      sem_current_mod = mod;
      resolve_scopes(&arena, mod, &ss, &sem);
      entry = entry->next;
    }
  }
  printf("Scope resolution complete.\n");

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      sem_current_mod = mod;
      type_check_ast(&arena, mod->ast_root, &sem);
      entry = entry->next;
    }
  }
  printf("Type checking complete.\n");

  // Write caches (now with typed ASTs)
  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = (Module *)entry->value;
      if (mod->needs_cache_write) {
        char cache_file[512];
        uint64_t path_hash = hash_string(mod->abs_path, strlen(mod->abs_path));
        snprintf(cache_file, sizeof(cache_file), ".tx_cache/%lu.cache",
                 path_hash);
        FILE *out = fopen(cache_file, "wb");
        if (out) {
          fwrite(&mod->content_hash, sizeof(uint64_t), 1, out);
          fclose(out);
          cache_write_ast(cache_file, mod->ast_root, mod->content,
                          mod->content_hash);
        }
      }
      entry = entry->next;
    }
  }

  const char *abs_path = resolve_alloc(&arena, opts->input_file);
  Module *main_mod = map_get(&sem.mod_cache, abs_path, strlen(abs_path));

  for (size_t i = 0; i < sem.mod_cache.capacity; i++) {
    HashEntry *entry = sem.mod_cache.buckets[i];
    while (entry) {
      Module *mod = entry->value;
      if (strcmp(mod->abs_path, abs_path) == 0) {
        printf("Transpiling main module: %s\n", opts->input_file);
        const char *base = strrchr(opts->input_file, '/');
        base = base ? base + 1 : opts->input_file;

        const char *bin_name;
        char derived_name[256];
        if (opts->as.build.output_file) {
          bin_name = opts->as.build.output_file;
        } else {
          strncpy(derived_name, base, sizeof(derived_name) - 1);
          derived_name[sizeof(derived_name) - 1] = '\0';
          char *dot = strrchr(derived_name, '.');
          if (dot && strcmp(dot, ".tx") == 0)
            *dot = '\0';
          bin_name = derived_name;
        }

        const char *std_flags[] = {"-O3", "-flto", "-Wno-strict-prototypes",
                                   "-Wextra", "-Wpedantic"};
        int total_flags = 5 + opts->as.build.extra_cflag_count;
        const char **all_flags =
            arena_alloc(&arena, (total_flags + 1) * sizeof(const char *));
        int idx = 0;
        for (int i = 0; i < 5; i++)
          all_flags[idx++] = std_flags[i];
        for (unsigned int i = 0; i < opts->as.build.extra_cflag_count; i++)
          all_flags[idx++] = opts->as.build.extra_cflags[i];
        all_flags[idx] = NULL;

        bool suc = output_to_c_and_compile(
            &sem, bin_name, opts->as.build.compiler, all_flags, total_flags,
            &arena, main_mod, opts->as.build.keep_c_files);
        if (suc)
          printf("Compiled successfully\n");
        else
          fprintf(stderr, "Failed to compile %s\n", opts->input_file);
      }
      entry = entry->next;
    }
  }

  if (pending.paths)
    free((void *)pending.paths);
  sem_deinit(&sem);
  arena_free_all(&arena);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--lsp") == 0) {
    start_lsp_server();
    return 0;
  }

  CompileOptions opts;
  if (parse_options(argc, argv, &opts) != 0) {
    print_usage(argv[0]);
    return 1;
  }
  if (opts.help) {
    print_usage(argv[0]);
    return 0;
  }

  if (opts.cmd == CMD_BUILD)
    compile_project(&opts);
  else if (opts.cmd == CMD_FMT)
    fmt_project(&opts);
  return 0;
}
