#include "ast_types.h"
#include "lex_types.h"
#include "parse_types.h"
#include "util.h"
#include <string.h>
#ifdef ENABLE_AST_COMPRESSION
#include <zstd.h>
#endif

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

typedef struct {
  AstNode *ptr;
  uint32_t idx;
} PtrMapEntry;

typedef struct {
  PtrMapEntry *entries;
  uint32_t cap;
  uint32_t count;
} PtrMap;

static void ptrmap_init(PtrMap *map, uint32_t cap) {
  map->cap = cap;
  map->count = 0;
  map->entries = calloc(cap, sizeof(PtrMapEntry));
}

static void ptrmap_expand(PtrMap *map) {
  uint32_t old_cap = map->cap;
  PtrMapEntry *old_entries = map->entries;
  map->cap = old_cap * 2;
  map->entries = calloc(map->cap, sizeof(PtrMapEntry));
  map->count = 0;
  for (uint32_t i = 0; i < old_cap; i++) {
    if (old_entries[i].ptr) {
      uint32_t idx =
          (((uintptr_t)old_entries[i].ptr >> 3) * 2654435761u) % map->cap;
      while (map->entries[idx].ptr != NULL)
        idx = (idx + 1) % map->cap;
      map->entries[idx] = old_entries[i];
      map->count++;
    }
  }
  free(old_entries);
}

static bool ptrmap_put(PtrMap *map, AstNode *ptr, uint32_t idx) {
  if (!ptr)
    return false;
  if (map->count * 2 >= map->cap)
    ptrmap_expand(map);
  uint32_t i = (((uintptr_t)ptr >> 3) * 2654435761u) % map->cap;
  while (map->entries[i].ptr != NULL) {
    if (map->entries[i].ptr == ptr)
      return false;
    i = (i + 1) % map->cap;
  }
  map->entries[i].ptr = ptr;
  map->entries[i].idx = idx;
  map->count++;
  return true;
}

static uint32_t ptrmap_get(PtrMap *map, AstNode *ptr) {
  if (!ptr || map->cap == 0)
    return 0xFFFFFFFF;
  uint32_t i = (((uintptr_t)ptr >> 3) * 2654435761u) % map->cap;
  while (map->entries[i].ptr != NULL) {
    if (map->entries[i].ptr == ptr)
      return map->entries[i].idx;
    i = (i + 1) % map->cap;
  }
  return 0xFFFFFFFF;
}

typedef struct {
  uint8_t *data;
  size_t size;
  size_t cap;
} ByteBuffer;

void buf_append(ByteBuffer *buf, const void *data, size_t len) {
  if (buf->size + len > buf->cap) {
    buf->cap = (buf->cap == 0) ? 4096 : buf->cap;
    while (buf->size + len > buf->cap)
      buf->cap *= 2;
    buf->data = realloc(buf->data, buf->cap);
  }
  memcpy(buf->data + buf->size, data, len);
  buf->size += len;
}

void cache_write_ast(const char *cache_path, AstNode *root,
                     const char *source_base) {
  if (!root)
    return;

  uint32_t q_cap = 1024;
  AstNode **queue = malloc(q_cap * sizeof(AstNode *));
  uint32_t head = 0, tail = 0;

  PtrMap map;
  ptrmap_init(&map, 2048);

#define PUSH_Q(n)                                                              \
  do {                                                                         \
    if ((n) && ptrmap_put(&map, (n), tail)) {                                  \
      if (tail >= q_cap) {                                                     \
        q_cap *= 2;                                                            \
        queue = realloc(queue, q_cap * sizeof(AstNode *));                     \
      }                                                                        \
      queue[tail++] = (n);                                                     \
    }                                                                          \
  } while (0)

  PUSH_Q(root);

  // BFS
  while (head < tail) {
    AstNode *node = queue[head++];
    PUSH_Q(node->next);

    // Enqueue EvalType arrays
    if (node->eval_type.array_dimens > 0 && node->eval_type.dim_sizes != NULL) {
      for (uint32_t d = 0; d < node->eval_type.array_dimens; d++)
        PUSH_Q(node->eval_type.dim_sizes[d]);
    }

    DataType *dt2 = NULL;
    switch (node->type) {
    case AST_BINOP:
      PUSH_Q(node->as.binop.left);
      PUSH_Q(node->as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PUSH_Q(node->as.unop.operand);
      break;
    case AST_ARRAY_LIT:
      PUSH_Q(node->as.array_lit.elements);
      break;
    case AST_VAR_DECL:
      PUSH_Q(node->as.var_decl.init);
      dt2 = &node->as.var_decl.type;
      break;
    case AST_IF:
      PUSH_Q(node->as.if_check.check);
      PUSH_Q(node->as.if_check.action);
      PUSH_Q(node->as.if_check.elseAct);
      break;
    case AST_STRUCT:
      PUSH_Q(node->as.struct_def.contents);
      break;
    case AST_UNION:
      PUSH_Q(node->as.union_def.contents);
      break;
    case AST_ENUM:
      PUSH_Q(node->as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      PUSH_Q(node->as.enum_member.val);
      break;
    case AST_DEFER:
      PUSH_Q(node->as.defer_stmt.contents);
      break;
    case AST_FOR:
      PUSH_Q(node->as.for_loop.init);
      PUSH_Q(node->as.for_loop.check);
      PUSH_Q(node->as.for_loop.inc);
      PUSH_Q(node->as.for_loop.action);
      break;
    case AST_WHILE:
      PUSH_Q(node->as.while_loop.check);
      PUSH_Q(node->as.while_loop.action);
      break;
    case AST_FUNC:
      PUSH_Q(node->as.func_def.params);
      PUSH_Q(node->as.func_def.block);
      dt2 = &node->as.func_def.ret_type;
      break;
    case AST_PARAM:
      dt2 = &node->as.fn_param.type;
      break;
    case AST_RET:
      PUSH_Q(node->as.ret_stmt.expr);
      break;
    case AST_BLOCK:
      PUSH_Q(node->as.block.first_stmt);
      break;
    case AST_FUNC_CALL:
      PUSH_Q(node->as.func_call.caller);
      PUSH_Q(node->as.func_call.args);
      break;
    case AST_INDEX:
      PUSH_Q(node->as.index.base);
      PUSH_Q(node->as.index.index);
      break;
    case AST_MEMBER:
      PUSH_Q(node->as.member.base);
      break;
    case AST_SWITCH:
      PUSH_Q(node->as.switch_stmt.check);
      PUSH_Q(node->as.switch_stmt.cases);
      PUSH_Q(node->as.switch_stmt.default_case);
      break;
    case AST_CASE:
      PUSH_Q(node->as.case_stmt.val);
      PUSH_Q(node->as.case_stmt.action);
      break;
    case AST_EXTERN:
      PUSH_Q(node->as.extern_block.contents);
      break;
    case AST_CAST:
      PUSH_Q(node->as.cast.op);
      dt2 = &node->as.cast.target;
      break;
    case AST_SIZEOF:
      PUSH_Q(node->as.sizeof_expr.target_expr);
      dt2 = &node->as.sizeof_expr.target_type;
      break;
    case AST_PROGRAM:
      PUSH_Q(node->as.block.first_stmt);
      break;
    default:
      break;
    }

    if (dt2 != NULL && dt2->array_dimens > 0 && dt2->dim_sizes != NULL) {
      for (uint32_t d = 0; d < dt2->array_dimens; d++)
        PUSH_Q(dt2->dim_sizes[d]);
    }
  }

  ByteBuffer out_buf = {0};

  uint32_t node_count = tail;
  buf_append(&out_buf, &node_count, sizeof(uint32_t));

// Tokens mapping to file offset
#define P_TOK(tok)                                                             \
  do {                                                                         \
    if ((tok).start && (tok).start >= source_base &&                           \
        (tok).start < source_base + strlen(source_base)) {                     \
      (tok).start = (const char *)(size_t)((tok).start - source_base);         \
    } else {                                                                   \
      (tok).start = (const char *)(size_t)(-1);                                \
    }                                                                          \
  } while (0)

  for (uint32_t i = 0; i < node_count; i++) {
    AstNode flat = *queue[i];

    flat.next = (AstNode *)(size_t)ptrmap_get(&map, flat.next);
    P_TOK(flat.eval_type.name);

    AstNode **dim1 = flat.eval_type.dim_sizes;
    uint32_t len1 = flat.eval_type.array_dimens;
    AstNode **dim2 = NULL;
    uint32_t len2 = 0;
    flat.eval_type.dim_sizes = NULL;

    switch (flat.type) {
    case AST_NUM_LIT:
      P_TOK(flat.as.num_lit.val);
      break;
    case AST_STR_LIT:
      P_TOK(flat.as.str_lit.val);
      break;
    case AST_CHAR_LIT:
      P_TOK(flat.as.char_lit.val);
      break;
    case AST_BOOL_LIT:
      P_TOK(flat.as.bool_lit.val);
      break;
    case AST_ARRAY_LIT:
      flat.as.array_lit.elements =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.array_lit.elements);
      break;
    case AST_IDENTIF:
      P_TOK(flat.as.identif.val);
      flat.as.identif.res_sm = NULL;
      break;
    case AST_BINOP:
      P_TOK(flat.as.binop.op);
      flat.as.binop.left =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.binop.left);
      flat.as.binop.right =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      P_TOK(flat.as.unop.op);
      flat.as.unop.operand =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.unop.operand);
      break;
    case AST_VAR_DECL:
      P_TOK(flat.as.var_decl.id);
      P_TOK(flat.as.var_decl.type.name);
      flat.as.var_decl.init =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.var_decl.init);
      dim2 = flat.as.var_decl.type.dim_sizes;
      len2 = flat.as.var_decl.type.array_dimens;
      flat.as.var_decl.type.dim_sizes = NULL;
      break;
    case AST_IF:
      P_TOK(flat.as.if_check.if_stmt);
      flat.as.if_check.check =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.if_check.check);
      flat.as.if_check.action =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.if_check.action);
      flat.as.if_check.elseAct =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.if_check.elseAct);
      break;
    case AST_STRUCT:
      P_TOK(flat.as.struct_def.structn);
      flat.as.struct_def.contents =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.struct_def.contents);
      break;
    case AST_UNION:
      P_TOK(flat.as.union_def.unionn);
      flat.as.union_def.contents =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.union_def.contents);
      break;
    case AST_ENUM:
      P_TOK(flat.as.enum_def.enumn);
      flat.as.enum_def.contents =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      P_TOK(flat.as.enum_member.name);
      flat.as.enum_member.val =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.enum_member.val);
      break;
    case AST_DEFER:
      P_TOK(flat.as.defer_stmt.defer);
      flat.as.defer_stmt.contents =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.defer_stmt.contents);
      break;
    case AST_FOR:
      P_TOK(flat.as.for_loop.for_stmt);
      flat.as.for_loop.init =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.for_loop.init);
      flat.as.for_loop.check =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.for_loop.check);
      flat.as.for_loop.inc =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.for_loop.inc);
      flat.as.for_loop.action =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.for_loop.action);
      break;
    case AST_WHILE:
      P_TOK(flat.as.while_loop.while_stmt);
      flat.as.while_loop.check =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.while_loop.check);
      flat.as.while_loop.action =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.while_loop.action);
      break;
    case AST_FUNC:
      P_TOK(flat.as.func_def.fn_name);
      P_TOK(flat.as.func_def.ret_type.name);
      flat.as.func_def.params =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.func_def.params);
      flat.as.func_def.block =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.func_def.block);
      dim2 = flat.as.func_def.ret_type.dim_sizes;
      len2 = flat.as.func_def.ret_type.array_dimens;
      flat.as.func_def.ret_type.dim_sizes = NULL;
      break;
    case AST_PARAM:
      P_TOK(flat.as.fn_param.id);
      P_TOK(flat.as.fn_param.type.name);
      dim2 = flat.as.fn_param.type.dim_sizes;
      len2 = flat.as.fn_param.type.array_dimens;
      flat.as.fn_param.type.dim_sizes = NULL;
      break;
    case AST_RET:
      P_TOK(flat.as.ret_stmt.ret_kw);
      flat.as.ret_stmt.expr =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.ret_stmt.expr);
      break;
    case AST_BLOCK:
      flat.as.block.first_stmt =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.block.first_stmt);
      break;
    case AST_FUNC_CALL:
      flat.as.func_call.caller =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.func_call.caller);
      flat.as.func_call.args =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.func_call.args);
      break;
    case AST_INDEX:
      flat.as.index.base =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.index.base);
      flat.as.index.index =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.index.index);
      break;
    case AST_MEMBER:
      flat.as.member.base =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.member.base);
      P_TOK(flat.as.member.name);
      P_TOK(flat.as.member.type);
      break;
    case AST_SWITCH:
      flat.as.switch_stmt.check =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.switch_stmt.check);
      flat.as.switch_stmt.cases =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.switch_stmt.cases);
      flat.as.switch_stmt.default_case =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.switch_stmt.default_case);
      break;
    case AST_CASE:
      flat.as.case_stmt.val =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.case_stmt.val);
      flat.as.case_stmt.action =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.case_stmt.action);
      break;
    case AST_EXTERN:
      flat.as.extern_block.contents =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.extern_block.contents);
      break;
    case AST_USE:
      P_TOK(flat.as.use_stmt.path);
      P_TOK(flat.as.use_stmt.alias);
      P_TOK(flat.as.use_stmt.use_kw);
      P_TOK(flat.as.use_stmt.semicln);
      break;
    case AST_NULL_LIT:
      P_TOK(flat.as.null_lit.val);
      break;
    case AST_BREAK:
      P_TOK(flat.as.break_stmt.kw);
      break;
    case AST_CONTINUE:
      P_TOK(flat.as.continue_stmt.kw);
      break;
    case AST_CAST:
      P_TOK(flat.as.cast.target.name);
      flat.as.cast.op = (AstNode *)(size_t)ptrmap_get(&map, flat.as.cast.op);
      dim2 = flat.as.cast.target.dim_sizes;
      len2 = flat.as.cast.target.array_dimens;
      flat.as.cast.target.dim_sizes = NULL;
      break;
    case AST_SIZEOF:
      P_TOK(flat.as.sizeof_expr.target_type.name);
      flat.as.sizeof_expr.target_expr =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.sizeof_expr.target_expr);
      dim2 = flat.as.sizeof_expr.target_type.dim_sizes;
      len2 = flat.as.sizeof_expr.target_type.array_dimens;
      flat.as.sizeof_expr.target_type.dim_sizes = NULL;
      break;
    case AST_PROGRAM:
      flat.as.block.first_stmt =
          (AstNode *)(size_t)ptrmap_get(&map, flat.as.block.first_stmt);
      break;
    default:
      break;
    }

    buf_append(&out_buf, &flat, sizeof(AstNode));

    if (len1 > 0) {
      for (uint32_t d = 0; d < len1; d++) {
        uint32_t idx =
            (dim1 && dim1[d]) ? ptrmap_get(&map, dim1[d]) : 0xFFFFFFFF;
        buf_append(&out_buf, &idx, sizeof(uint32_t));
      }
    }
    if (len2 > 0) {
      for (uint32_t d = 0; d < len2; d++) {
        uint32_t idx =
            (dim2 && dim2[d]) ? ptrmap_get(&map, dim2[d]) : 0xFFFFFFFF;
        buf_append(&out_buf, &idx, sizeof(uint32_t));
      }
    }
  }

  FILE *fp = fopen(cache_path, "wb");
  if (!fp) {
    free(queue);
    free(map.entries);
    free(out_buf.data);
    return;
  }

#ifdef ENABLE_AST_COMPRESSION
  size_t cBuffSize = ZSTD_compressBound(out_buf.size);
  void *cBuff = malloc(cBuffSize);
  size_t cSize = ZSTD_compress(cBuff, cBuffSize, out_buf.data, out_buf.size, 1);

  if (!ZSTD_isError(cSize)) {
    uint32_t magic = 0x5A4D4341; // 'ZMCA' - Zstd Compressed Ast
    uint64_t uncomp_size = out_buf.size;
    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&uncomp_size, sizeof(uint64_t), 1, fp);
    fwrite(cBuff, 1, cSize, fp);
  } else {
    printf("ZSTD Compression failed!\n");
  }
  free(cBuff);
#else
  uint32_t magic = 0x554E4341; // 'UNCA' - Uncompressed Ast
  fwrite(&magic, sizeof(uint32_t), 1, fp);
  fwrite(out_buf.data, 1, out_buf.size, fp);
#endif

  fclose(fp);
  free(out_buf.data);
  free(queue);
  free(map.entries);
}

AstNode *cache_read_ast(Arena *arena, const char *cache_path,
                        const char *source_base) {
  FILE *fp = fopen(cache_path, "rb");
  if (!fp)
    return NULL;

  uint32_t magic;
  if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) {
    fclose(fp);
    return NULL;
  }

  void *read_buffer = NULL;
  size_t read_offset = 0;

  if (magic == 0x5A4D4341) { // Compressed
#ifdef ENABLE_AST_COMPRESSION
    uint64_t uncomp_size;
    if (fread(&uncomp_size, sizeof(uint64_t), 1, fp) != 1) {
      fclose(fp);
      return NULL;
    }

    // Read the remaining compressed data
    fseek(fp, 0, SEEK_END);
    long comp_size = ftell(fp) - sizeof(uint32_t) - sizeof(uint64_t);
    fseek(fp, sizeof(uint32_t) + sizeof(uint64_t), SEEK_SET);

    void *comp_buf = malloc(comp_size);
    if (fread(comp_buf, 1, comp_size, fp) != 1) {
      free(comp_buf);
      fclose(fp);
      return NULL;
    }
    fclose(fp); // Done with the file

    read_buffer = malloc(uncomp_size);
    size_t dSize =
        ZSTD_decompress(read_buffer, uncomp_size, comp_buf, comp_size);
    free(comp_buf);

    if (ZSTD_isError(dSize)) {
      free(read_buffer);
      return NULL;
    }
#else
    fprintf(stderr, "Error: Cache is compressed but ENABLE_AST_COMPRESSION is off.\n");
    fclose(fp);
    return NULL;
#endif
  } else if (magic == 0x554E4341) { // Uncompressed
    fseek(fp, 0, SEEK_END);
    long uncomp_size = ftell(fp) - sizeof(uint32_t);
    fseek(fp, sizeof(uint32_t), SEEK_SET);

    read_buffer = malloc(uncomp_size);
    if (fread(read_buffer, 1, uncomp_size, fp) != 1) {
      free(read_buffer);
      fclose(fp);
      return NULL;
    }
    fclose(fp);
  } else {
    fclose(fp);
    return NULL; // Invalid or old cache format
  }

// Macro to read from our decompressed memory buffer instead of fread
#define READ_MEM(dest, size)                                                   \
  do {                                                                         \
    memcpy((dest), (char *)read_buffer + read_offset, (size));                 \
    read_offset += (size);                                                     \
  } while (0)

  uint32_t node_count = 0;
  READ_MEM(&node_count, sizeof(uint32_t));
  if (node_count == 0) {
    free(read_buffer);
    return NULL;
  }

  AstNode *nodes = arena_alloc(arena, node_count * sizeof(AstNode));

  for (uint32_t i = 0; i < node_count; i++) {
    READ_MEM(&nodes[i], sizeof(AstNode));

    if (nodes[i].eval_type.array_dimens > 0) {
      nodes[i].eval_type.dim_sizes = arena_alloc(
          arena, nodes[i].eval_type.array_dimens * sizeof(AstNode *));
      for (uint32_t d = 0; d < nodes[i].eval_type.array_dimens; d++) {
        uint32_t idx;
        READ_MEM(&idx, sizeof(uint32_t));
        nodes[i].eval_type.dim_sizes[d] =
            (idx != 0xFFFFFFFF) ? &nodes[idx] : NULL;
      }
    }

    DataType *dt2 = NULL;
    switch (nodes[i].type) {
    case AST_VAR_DECL:
      dt2 = &nodes[i].as.var_decl.type;
      break;
    case AST_PARAM:
      dt2 = &nodes[i].as.fn_param.type;
      break;
    case AST_FUNC:
      dt2 = &nodes[i].as.func_def.ret_type;
      break;
    case AST_CAST:
      dt2 = &nodes[i].as.cast.target;
      break;
    case AST_SIZEOF:
      dt2 = &nodes[i].as.sizeof_expr.target_type;
      break;
    default:
      break;
    }

    if (dt2 && dt2->array_dimens > 0) {
      dt2->dim_sizes =
          arena_alloc(arena, dt2->array_dimens * sizeof(AstNode *));
      for (uint32_t d = 0; d < dt2->array_dimens; d++) {
        uint32_t idx;
        READ_MEM(&idx, sizeof(uint32_t));
        dt2->dim_sizes[d] = (idx != 0xFFFFFFFF) ? &nodes[idx] : NULL;
      }
    }
  }

  // Free the buffer now that we've deserialized everything into the arena
  free(read_buffer);

#define PATCH_STR(tok)                                                         \
  do {                                                                         \
    if ((size_t)(tok).start == (size_t)-1)                                     \
      (tok).start = NULL;                                                      \
    else                                                                       \
      (tok).start = source_base + (size_t)(tok).start;                         \
  } while (0)

#define PATCH_PTR(ptr)                                                         \
  do {                                                                         \
    if ((uintptr_t)(ptr) == 0xFFFFFFFF)                                        \
      (ptr) = NULL;                                                            \
    else                                                                       \
      (ptr) = &nodes[(uintptr_t)(ptr)];                                        \
  } while (0)

  for (uint32_t i = 0; i < node_count; i++) {
    AstNode *n = &nodes[i];
    PATCH_PTR(n->next);
    PATCH_STR(n->eval_type.name);

    switch (n->type) {
    case AST_NUM_LIT:
      PATCH_STR(n->as.num_lit.val);
      break;
    case AST_STR_LIT:
      PATCH_STR(n->as.str_lit.val);
      break;
    case AST_CHAR_LIT:
      PATCH_STR(n->as.char_lit.val);
      break;
    case AST_BOOL_LIT:
      PATCH_STR(n->as.bool_lit.val);
      break;
    case AST_ARRAY_LIT:
      PATCH_PTR(n->as.array_lit.elements);
      break;
    case AST_IDENTIF:
      PATCH_STR(n->as.identif.val);
      break;
    case AST_BINOP:
      PATCH_STR(n->as.binop.op);
      PATCH_PTR(n->as.binop.left);
      PATCH_PTR(n->as.binop.right);
      break;
    case AST_UOP:
    case AST_ADDR_OF:
    case AST_DEREF:
      PATCH_STR(n->as.unop.op);
      PATCH_PTR(n->as.unop.operand);
      break;
    case AST_VAR_DECL:
      PATCH_STR(n->as.var_decl.id);
      PATCH_STR(n->as.var_decl.type.name);
      PATCH_PTR(n->as.var_decl.init);
      break;
    case AST_IF:
      PATCH_STR(n->as.if_check.if_stmt);
      PATCH_PTR(n->as.if_check.check);
      PATCH_PTR(n->as.if_check.action);
      PATCH_PTR(n->as.if_check.elseAct);
      break;
    case AST_STRUCT:
      PATCH_STR(n->as.struct_def.structn);
      PATCH_PTR(n->as.struct_def.contents);
      break;
    case AST_UNION:
      PATCH_STR(n->as.union_def.unionn);
      PATCH_PTR(n->as.union_def.contents);
      break;
    case AST_ENUM:
      PATCH_STR(n->as.enum_def.enumn);
      PATCH_PTR(n->as.enum_def.contents);
      break;
    case AST_ENUM_MEMBER:
      PATCH_STR(n->as.enum_member.name);
      PATCH_PTR(n->as.enum_member.val);
      break;
    case AST_DEFER:
      PATCH_STR(n->as.defer_stmt.defer);
      PATCH_PTR(n->as.defer_stmt.contents);
      break;
    case AST_FOR:
      PATCH_STR(n->as.for_loop.for_stmt);
      PATCH_PTR(n->as.for_loop.init);
      PATCH_PTR(n->as.for_loop.check);
      PATCH_PTR(n->as.for_loop.inc);
      PATCH_PTR(n->as.for_loop.action);
      break;
    case AST_WHILE:
      PATCH_STR(n->as.while_loop.while_stmt);
      PATCH_PTR(n->as.while_loop.check);
      PATCH_PTR(n->as.while_loop.action);
      break;
    case AST_FUNC:
      PATCH_STR(n->as.func_def.fn_name);
      PATCH_STR(n->as.func_def.ret_type.name);
      PATCH_PTR(n->as.func_def.params);
      PATCH_PTR(n->as.func_def.block);
      break;
    case AST_PARAM:
      PATCH_STR(n->as.fn_param.id);
      if (n->as.fn_param.id.start == NULL && n->as.fn_param.id.len == 4) {
        n->as.fn_param.id.start = "self";
      }
      PATCH_STR(n->as.fn_param.type.name);
      break;
    case AST_RET:
      PATCH_STR(n->as.ret_stmt.ret_kw);
      PATCH_PTR(n->as.ret_stmt.expr);
      break;
    case AST_BLOCK:
      PATCH_PTR(n->as.block.first_stmt);
      break;
    case AST_FUNC_CALL:
      PATCH_PTR(n->as.func_call.caller);
      PATCH_PTR(n->as.func_call.args);
      break;
    case AST_INDEX:
      PATCH_PTR(n->as.index.base);
      PATCH_PTR(n->as.index.index);
      break;
    case AST_MEMBER:
      PATCH_PTR(n->as.member.base);
      PATCH_STR(n->as.member.name);
      PATCH_STR(n->as.member.type);
      break;
    case AST_SWITCH:
      PATCH_PTR(n->as.switch_stmt.check);
      PATCH_PTR(n->as.switch_stmt.cases);
      PATCH_PTR(n->as.switch_stmt.default_case);
      break;
    case AST_CASE:
      PATCH_PTR(n->as.case_stmt.val);
      PATCH_PTR(n->as.case_stmt.action);
      break;
    case AST_EXTERN:
      PATCH_PTR(n->as.extern_block.contents);
      break;
    case AST_USE:
      PATCH_STR(n->as.use_stmt.path);
      PATCH_STR(n->as.use_stmt.alias);
      PATCH_STR(n->as.use_stmt.use_kw);
      PATCH_STR(n->as.use_stmt.semicln);
      break;
    case AST_NULL_LIT:
      PATCH_STR(n->as.null_lit.val);
      break;
    case AST_BREAK:
      PATCH_STR(n->as.break_stmt.kw);
      break;
    case AST_CONTINUE:
      PATCH_STR(n->as.continue_stmt.kw);
      break;
    case AST_CAST:
      PATCH_STR(n->as.cast.target.name);
      PATCH_PTR(n->as.cast.op);
      break;
    case AST_SIZEOF:
      PATCH_STR(n->as.sizeof_expr.target_type.name);
      PATCH_PTR(n->as.sizeof_expr.target_expr);
      break;
    case AST_PROGRAM:
      PATCH_PTR(n->as.block.first_stmt);
      break;
    default:
      break;
    }
  }
  return &nodes[0];
}
