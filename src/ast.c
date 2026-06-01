#include "ast_types.h"
#include "lex_types.h"
#include "parse_types.h"
#include "util.h"

void init_lex_maps(LexCtx *ctx, Arena *arena) {
  // Capacities padded to powers of 2 for minimal collisions
  map_init(&ctx->kw_map, arena, 64);
  map_init(&ctx->op_map, arena, 64);
  map_init(&ctx->comp_map, arena, 16);
  map_init(&ctx->type_kw_map, arena, 64);

  for (size_t i = 0; i < kwlistlen; i++)
    map_set(&ctx->kw_map, kwlist[i], strlen(kwlist[i]), (void *)1);

  for (size_t i = 0; i < oplistlen; i++)
    map_set(&ctx->op_map, oplist[i], strlen(oplist[i]), (void *)1);

  for (size_t i = 0; i < complistlen; i++)
    map_set(&ctx->comp_map, complist[i], strlen(complist[i]), (void *)1);

  for (size_t i = 0; i < typelistlen; i++)
    map_set(&ctx->type_kw_map, typelist[i], strlen(typelist[i]), (void *)1);

  // Its not technically a keyword but its an inbuilt function
  map_set(&ctx->kw_map, "sizeof", 6, (void *)1);
}

AstNode *new_node(Arena *arena, ASTN_TYPE type) {
  AstNode *node = arena_alloc(arena, sizeof(AstNode));
  memset(node, 0, sizeof(AstNode));
  node->type = type;
  return node;
}

void write_ast(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "w")) != NULL) {
    // Write stuff here
    fclose(fp);
  } else {
    printf("Failed to open file to write AST");
  }
}

void append_stmt(AstNode **head, AstNode *new_stmt) {
  if (*head == NULL) {
    *head = new_stmt;
  } else {
    AstNode *tail = *head;
    while (tail->next != NULL) {
      tail = tail->next;
    }
    tail->next = new_stmt;
  }
}

AstNode *str_to_ast(Arena *arena, const char *file, const char *fpath,
                    DiagList *diag_list, bool partial) {
  LexCtx lex = {0};
  lex.start = (char *)file;
  lex.curr = (char *)file;
  lex.line = 1;
  lex.col = 1;
  lex.file = fpath;
  init_lex_maps(&lex, arena);

  ParseCtx pctx = {0};
  pctx.lex = &lex;
  pctx.arena = arena;
  pctx.state_cap = 64;
  pctx.state_stack = malloc(sizeof(ParseState) * pctx.state_cap);
  pctx.diags = diag_list;
  pctx.curr = next_token(&pctx);

  AstNode *root = new_node(arena, AST_PROGRAM);
  push_node(&pctx, root);

  bool success = parse(&pctx);
  map_free_buckets(&lex.kw_map);
  map_free_buckets(&lex.op_map);
  map_free_buckets(&lex.comp_map);
  map_free_buckets(&lex.type_kw_map);
  free(pctx.state_stack);
  free(pctx.node_stack);
  free(pctx.op_stack);

  if (!success && !partial) {
    return NULL;
  }
  return root;
}

AstNode *file_to_ast(Arena *arena, const char *path, bool partial) {
  const char *file = load_file(path);
  if (!file)
    return NULL;
  DiagList diags;
  diaglist_init(&diags, 1024);
  AstNode *root = str_to_ast(arena, file, path, &diags, partial);
  if (!partial) {
    for (size_t i = 0; i < diags.count; i++) {
      printf("Error on %u:%u in file %s: %s\n", diags.items[i].start_line,
             diags.items[i].start_char, diags.items[i].file,
             diags.items[i].message);
    }
  }
  diaglist_free(&diags);
  return root;
}
