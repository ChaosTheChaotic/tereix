#include "ast_serde.h"
#include "ast_visitor.h"
#include "hashmap.h"
#include "hashutils.h"
#include <string.h>
#ifdef ENABLE_AST_COMPRESSION
#include <zstd.h>
#endif

typedef struct {
  AstNode *ptr;
  uint32_t idx;
} PtrMapEntry;

typedef struct {
  PtrMapEntry *entries;
  uint32_t cap;
  uint32_t count;
} PtrMap;

void ptrmap_init(PtrMap *map, uint32_t cap) {
  map->cap = cap;
  map->count = 0;
  map->entries = calloc(cap, sizeof(PtrMapEntry));
}

void ptrmap_expand(PtrMap *map) {
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

bool ptrmap_put(PtrMap *map, AstNode *ptr, uint32_t idx) {
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

uint32_t ptrmap_get(PtrMap *map, AstNode *ptr) {
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

bool buf_append(ByteBuffer *buf, const void *data, size_t len) {
  if (buf->size + len > buf->cap) {
    size_t new_cap = (buf->cap == 0) ? 4096 : buf->cap;
    while (buf->size + len > new_cap)
      new_cap *= 2;

    uint8_t *new_data = realloc(buf->data, new_cap);
    if (!new_data) {
      fprintf(stderr,
              "Serialization Error: Out of memory writing to ByteBuffer.\n");
      return false;
    }
    buf->data = new_data;
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->size, data, len);
  buf->size += len;
  return true;
}

void cache_write_ast(const char *cache_path, AstNode *root,
                     const char *source_base, uint64_t content_hash) {
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
        size_t new_q_cap = q_cap * 2;                                          \
        AstNode **new_queue = realloc(queue, new_q_cap * sizeof(AstNode *));       \
        if (!new_queue) {                                                      \
          fprintf(stderr, "Failed to realloc stack during cache write\n");       \
          free(queue);                                                         \
          return;                                                              \
        }                                                                      \
        queue = new_queue;                                                     \
        q_cap = new_q_cap;                                                     \
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
  if (!buf_append(&out_buf, &node_count, sizeof(uint32_t))) {
    free(queue);
    free(map.entries);
    return;
  }

// Tokens mapping to file offset
#define P_TOK(tok)                                                             \
  do {                                                                         \
    if ((tok).start && (tok).start >= source_base &&                           \
        (tok).start <= source_base + strlen(source_base)) {                    \
      (tok).start = (const char *)(size_t)((tok).start - source_base);         \
    } else if ((tok).start) {                                                  \
      bool _found = false;                                                     \
      for (size_t _k = 0; _k < typelistlen; _k++) {                            \
        if (strcmp((tok).start, typelist[_k]) == 0) {                          \
          (tok).start = (const char *)(size_t)(0xFFFFFF00 + _k);               \
          _found = true;                                                       \
          break;                                                               \
        }                                                                      \
      }                                                                        \
      if (!_found)                                                             \
        (tok).start = (const char *)(size_t)(-1);                              \
    } else {                                                                   \
      (tok).start = (const char *)(size_t)(-1);                                \
    }                                                                          \
  } while (0)

#define P_RAW_PTR(ptr)                                                         \
  do {                                                                         \
    if ((ptr) && (ptr) >= source_base &&                                       \
        (ptr) < source_base + strlen(source_base)) {                           \
      (ptr) = (const char *)(size_t)((ptr) - source_base);                     \
    } else {                                                                   \
      (ptr) = (const char *)(size_t)(-1);                                      \
    }                                                                          \
  } while (0)

  for (uint32_t i = 0; i < node_count; i++) {
    AstNode flat = *queue[i];

    flat.next = (AstNode *)(size_t)ptrmap_get(&map, flat.next);
    P_TOK(flat.eval_type.name);

    P_RAW_PTR(flat.src_start);
    P_RAW_PTR(flat.src_end);

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

    if (!buf_append(&out_buf, &flat, sizeof(AstNode))) {
      free(queue);
      free(map.entries);
      free(out_buf.data);
      return;
    }

    if (len1 > 0) {
      for (uint32_t d = 0; d < len1; d++) {
        uint32_t idx =
            (dim1 && dim1[d]) ? ptrmap_get(&map, dim1[d]) : 0xFFFFFFFF;
        if (!buf_append(&out_buf, &idx, sizeof(uint32_t))) {
          free(queue);
          free(map.entries);
          free(out_buf.data);
          return;
        }
      }
    }
    if (len2 > 0) {
      for (uint32_t d = 0; d < len2; d++) {
        uint32_t idx =
            (dim2 && dim2[d]) ? ptrmap_get(&map, dim2[d]) : 0xFFFFFFFF;
        if (!buf_append(&out_buf, &idx, sizeof(uint32_t))) {
          free(queue);
          free(map.entries);
          free(out_buf.data);
          return;
        }
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

  fwrite(&content_hash, sizeof(uint64_t), 1, fp);

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
                        const char *source_base, size_t skip_bytes) {
  FILE *fp = fopen(cache_path, "rb");
  if (!fp)
    return NULL;

  if (skip_bytes > 0 && fseek(fp, skip_bytes, SEEK_SET) != 0) {
    fclose(fp);
    return NULL;
  }

  uint32_t magic;
  if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) {
    fclose(fp);
    return NULL;
  }

  if (magic != 0x5A4D4341 && magic != 0x554E4341) {
    fprintf(stderr, "Error reading ast: invalid magic 0x%08X\n", magic);
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

    // Current position = start of compressed data
    long data_start = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    long comp_size = file_size - data_start;
    fseek(fp, data_start, SEEK_SET);

    void *comp_buf = malloc(comp_size);
    if (fread(comp_buf, 1, comp_size, fp) != (unsigned long)comp_size) {
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
    fprintf(stderr,
            "Error: Cache is compressed but ENABLE_AST_COMPRESSION is off.\n");
    fclose(fp);
    return NULL;
#endif
  } else if (magic == 0x554E4341) { // Uncompressed
    long data_start = ftell(fp);    // after magic
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    long data_size = file_size - data_start;
    fseek(fp, data_start, SEEK_SET);

    read_buffer = malloc(data_size);
    if (fread(read_buffer, 1, data_size, fp) != (size_t)data_size) {
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

  if (node_count == 0) {
    fprintf(stderr, "Error reading ast: node_count is 0\n");
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

#define PATCH_STR(tok)                                                         \
  do {                                                                         \
    size_t _val = (size_t)(tok).start;                                         \
    if (_val >= 0xFFFFFF00 && _val < 0xFFFFFF00 + typelistlen) {               \
      (tok).start = typelist[_val - 0xFFFFFF00];                               \
    } else if (_val == (size_t)-1) {                                           \
      (tok).start = NULL;                                                      \
    } else {                                                                   \
      (tok).start = source_base + _val;                                        \
    }                                                                          \
  } while (0)

#define PATCH_PTR(ptr)                                                         \
  do {                                                                         \
    uintptr_t _idx = (uintptr_t)(ptr);                                         \
    if (_idx == 0xFFFFFFFF)                                                    \
      (ptr) = NULL;                                                            \
    else if (_idx >= node_count) {                                             \
      fprintf(stderr, "Error: PATCH_PTR index %zu out of range\n", _idx);      \
      free(read_buffer);                                                       \
      return NULL;                                                             \
    } else                                                                     \
      (ptr) = &nodes[_idx];                                                    \
  } while (0)

#define PATCH_RAW_PTR(ptr)                                                     \
  do {                                                                         \
    if ((size_t)(ptr) == (size_t)-1)                                           \
      (ptr) = NULL;                                                            \
    else                                                                       \
      (ptr) = source_base + (size_t)(ptr);                                     \
  } while (0)

  for (uint32_t i = 0; i < node_count; i++) {
    AstNode *n = &nodes[i];
    PATCH_PTR(n->next);
    PATCH_STR(n->eval_type.name);

    PATCH_RAW_PTR(n->src_start);
    PATCH_RAW_PTR(n->src_end);

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
      if (((size_t)n->as.fn_param.id.start == (size_t)-1 ||
           n->as.fn_param.id.start == NULL) &&
          n->as.fn_param.id.len == 4) {
        n->as.fn_param.id.start = "self";
        n->as.fn_param.id.type = TOKEN_IDENTIF;
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
  // Free the buffer now that weve deserialized everything into the arena
  free(read_buffer);
  return &nodes[0];
}

inline uint32_t hash_token(uint32_t hash, Token tok) {
  hash = combine_hash(hash, tok.type);
  if (tok.start && tok.len > 0 && (size_t)tok.start != (size_t)-1) {
    uint32_t str_hash = hash_string(tok.start, tok.len);
    hash = combine_hash(hash, str_hash);
  }
  return hash;
}

inline uint32_t hash_datatype(uint32_t hash, DataType dt) {
  hash = hash_token(hash, dt.name);
  hash = combine_hash(hash, dt.ptr_depth);
  hash = combine_hash(hash, dt.array_dimens);
  hash = combine_hash(hash, dt.is_static);
  hash = combine_hash(hash, dt.is_mut);
  hash = combine_hash(hash, dt.is_threadlocal);
  hash = combine_hash(hash, dt.is_extern);
  return hash;
}

VisitResult compute_hash_enter(AstVisitor *visitor, AstNode *n) {
	(void)visitor; // Unused
  // If the hash is already computed skip traversing its children
  if (n->node_hash) {
    return VISIT_SKIP_CHILDREN;
  }
  return VISIT_CONTINUE;
}

void compute_hash_exit(AstVisitor *visitor, AstNode *n) {
	(void)visitor; // Unused
  // ast_traverse still calls exit_node even if VISIT_SKIP_CHILDREN was returned so stop recomputing
  if (n->node_hash) return;

  uint32_t hash = combine_hash(2166136261u, n->type);

  switch (n->type) {
  case AST_BINOP:
    hash = hash_token(hash, n->as.binop.op);
    if (n->as.binop.left) hash = combine_hash(hash, n->as.binop.left->node_hash);
    if (n->as.binop.right) hash = combine_hash(hash, n->as.binop.right->node_hash);
    break;
  case AST_UOP:
  case AST_ADDR_OF:
  case AST_DEREF:
    hash = hash_token(hash, n->as.unop.op);
    hash = combine_hash(hash, n->as.unop.is_postfix);
    if (n->as.unop.operand) hash = combine_hash(hash, n->as.unop.operand->node_hash);
    break;
  case AST_ARRAY_LIT:
    for (AstNode *e = n->as.array_lit.elements; e; e = e->next)
      hash = combine_hash(hash, e->node_hash);
    break;
  case AST_VAR_DECL:
    hash = hash_token(hash, n->as.var_decl.id);
    hash = hash_datatype(hash, n->as.var_decl.type);
    if (n->as.var_decl.init)
      hash = combine_hash(hash, n->as.var_decl.init->node_hash);
    break;
  case AST_IF:
    if (n->as.if_check.check) hash = combine_hash(hash, n->as.if_check.check->node_hash);
    if (n->as.if_check.action) hash = combine_hash(hash, n->as.if_check.action->node_hash);
    if (n->as.if_check.elseAct) hash = combine_hash(hash, n->as.if_check.elseAct->node_hash);
    break;
  case AST_STRUCT:
    hash = hash_token(hash, n->as.struct_def.structn);
    hash = combine_hash(hash, n->as.struct_def.is_extern);
    for (AstNode *c = n->as.struct_def.contents; c; c = c->next)
      hash = combine_hash(hash, c->node_hash);
    break;
  case AST_UNION:
    hash = hash_token(hash, n->as.union_def.unionn);
    hash = combine_hash(hash, n->as.union_def.is_extern);
    for (AstNode *c = n->as.union_def.contents; c; c = c->next)
      hash = combine_hash(hash, c->node_hash);
    break;
  case AST_ENUM:
    hash = hash_token(hash, n->as.enum_def.enumn);
    for (AstNode *c = n->as.enum_def.contents; c; c = c->next)
      hash = combine_hash(hash, c->node_hash);
    break;
  case AST_ENUM_MEMBER:
    hash = hash_token(hash, n->as.enum_member.name);
    if (n->as.enum_member.val) hash = combine_hash(hash, n->as.enum_member.val->node_hash);
    break;
  case AST_DEFER:
    if (n->as.defer_stmt.contents) hash = combine_hash(hash, n->as.defer_stmt.contents->node_hash);
    break;
  case AST_FOR:
    if (n->as.for_loop.init) hash = combine_hash(hash, n->as.for_loop.init->node_hash);
    if (n->as.for_loop.check) hash = combine_hash(hash, n->as.for_loop.check->node_hash);
    if (n->as.for_loop.inc) hash = combine_hash(hash, n->as.for_loop.inc->node_hash);
    if (n->as.for_loop.action) hash = combine_hash(hash, n->as.for_loop.action->node_hash);
    break;
  case AST_WHILE:
    if (n->as.while_loop.check) hash = combine_hash(hash, n->as.while_loop.check->node_hash);
    if (n->as.while_loop.action) hash = combine_hash(hash, n->as.while_loop.action->node_hash);
    break;
  case AST_FUNC:
    hash = hash_token(hash, n->as.func_def.fn_name);
    hash = hash_datatype(hash, n->as.func_def.ret_type);
    hash = combine_hash(hash, n->as.func_def.is_extern);
    hash = combine_hash(hash, n->as.func_def.is_async);
    hash = combine_hash(hash, n->as.func_def.is_inline);
    for (AstNode *p = n->as.func_def.params; p; p = p->next)
      hash = combine_hash(hash, p->node_hash);
    if (n->as.func_def.block) hash = combine_hash(hash, n->as.func_def.block->node_hash);
    break;
  case AST_PARAM:
    hash = hash_token(hash, n->as.fn_param.id);
    hash = hash_datatype(hash, n->as.fn_param.type);
    break;
  case AST_RET:
    if (n->as.ret_stmt.expr) hash = combine_hash(hash, n->as.ret_stmt.expr->node_hash);
    break;
  case AST_BLOCK:
  case AST_PROGRAM:
    for (AstNode *s = n->as.block.first_stmt; s; s = s->next)
      hash = combine_hash(hash, s->node_hash);
    break;
  case AST_FUNC_CALL:
    if (n->as.func_call.caller) hash = combine_hash(hash, n->as.func_call.caller->node_hash);
    for (AstNode *a = n->as.func_call.args; a; a = a->next)
      hash = combine_hash(hash, a->node_hash);
    break;
  case AST_INDEX:
    if (n->as.index.base) hash = combine_hash(hash, n->as.index.base->node_hash);
    if (n->as.index.index) hash = combine_hash(hash, n->as.index.index->node_hash);
    break;
  case AST_MEMBER:
    hash = hash_token(hash, n->as.member.name);
    if (n->as.member.base) hash = combine_hash(hash, n->as.member.base->node_hash);
    break;
  case AST_SWITCH:
    if (n->as.switch_stmt.check) hash = combine_hash(hash, n->as.switch_stmt.check->node_hash);
    for (AstNode *c = n->as.switch_stmt.cases; c; c = c->next)
      hash = combine_hash(hash, c->node_hash);
    if (n->as.switch_stmt.default_case)
      hash = combine_hash(hash, n->as.switch_stmt.default_case->node_hash);
    break;
  case AST_CASE:
    if (n->as.case_stmt.val) hash = combine_hash(hash, n->as.case_stmt.val->node_hash);
    if (n->as.case_stmt.action) hash = combine_hash(hash, n->as.case_stmt.action->node_hash);
    break;
  case AST_EXTERN:
    for (AstNode *c = n->as.extern_block.contents; c; c = c->next)
      hash = combine_hash(hash, c->node_hash);
    break;
  case AST_USE:
    hash = hash_token(hash, n->as.use_stmt.path);
    if (n->as.use_stmt.alias.start)
      hash = hash_token(hash, n->as.use_stmt.alias);
    break;
  case AST_CAST:
    hash = hash_datatype(hash, n->as.cast.target);
    if (n->as.cast.op) hash = combine_hash(hash, n->as.cast.op->node_hash);
    break;
  case AST_SIZEOF:
    if (n->as.sizeof_expr.is_type)
      hash = hash_datatype(hash, n->as.sizeof_expr.target_type);
    else if (n->as.sizeof_expr.target_expr)
      hash = combine_hash(hash, n->as.sizeof_expr.target_expr->node_hash);
    break;
  case AST_IDENTIF:
  case AST_NUM_LIT:
  case AST_STR_LIT:
  case AST_CHAR_LIT:
  case AST_BOOL_LIT:
  case AST_NULL_LIT:
  case AST_BREAK:
  case AST_CONTINUE:
    hash = hash_token(hash, n->as.identif.val);
    break;
  default:
    break;
  }
  
  n->node_hash = hash;
}

uint32_t compute_node_hash(AstNode *root) {
  if (!root) return 0;

  AstVisitor visitor = {0};
  visitor.enter_node = compute_hash_enter;
  visitor.exit_node  = compute_hash_exit;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, root);
  } else {
    fprintf(stderr, "OOM encountered during hash computation.\n");
  }

  return root->node_hash;
}

typedef struct {
  Arena *arena;
  DeclMetadata *meta;
  HashMap *alias_map;
} ExtDeclDepData;

VisitResult ext_decl_dep_enter(AstVisitor *visitor, AstNode *node) {
  ExtDeclDepData *data = (ExtDeclDepData *)visitor->user_data;
  Arena *arena = data->arena;
  DeclMetadata *meta = data->meta;
  HashMap *alias_map = data->alias_map;

  if (node->type == AST_FUNC_CALL && node->as.func_call.caller) {
    Token dep_tok = {0};

    if (node->as.func_call.caller->type == AST_IDENTIF) {
      dep_tok = node->as.func_call.caller->as.identif.val;
    } else if (node->as.func_call.caller->type == AST_MEMBER) {
      AstNode *base = node->as.func_call.caller->as.member.base;
      Token mem_name = node->as.func_call.caller->as.member.name;

      // Format identifs
      if (base->type == AST_IDENTIF) {
        Token base_name = base->as.identif.val;

        // Attempt to resolve against local aliases
        if (alias_map) {
          Token *real_mod = map_get(alias_map, base_name.start, base_name.len);
          if (real_mod)
            base_name = *real_mod;
        }

        size_t combined_len = base_name.len + 1 + mem_name.len;
        char *combined = arena_alloc(arena, combined_len + 1);
        sprintf(combined, "%.*s_%.*s", (int)base_name.len, base_name.start,
                (int)mem_name.len, mem_name.start);

        dep_tok.start = combined;
        dep_tok.len = combined_len;
      } else {
        dep_tok = mem_name; // Fallback for complex member access
      }
    }

    if (dep_tok.start) {
      DepNode *dep = arena_alloc(arena, sizeof(DepNode));
      dep->name = dep_tok;
      dep->next = meta->calls_to;
      meta->calls_to = dep;
    }
  }

  // Extract Type Dependencies
  if (node->type == AST_VAR_DECL || node->type == AST_PARAM ||
      node->type == AST_FUNC || node->type == AST_CAST ||
      node->type == AST_SIZEOF) {
    DataType *dt = NULL;
    if (node->type == AST_VAR_DECL)
      dt = &node->as.var_decl.type;
    else if (node->type == AST_PARAM)
      dt = &node->as.fn_param.type;
    else if (node->type == AST_FUNC)
      dt = &node->as.func_def.ret_type;
    else if (node->type == AST_CAST)
      dt = &node->as.cast.target;
    else if (node->type == AST_SIZEOF && node->as.sizeof_expr.is_type)
      dt = &node->as.sizeof_expr.target_type;

    if (dt && dt->is_custom && dt->name.start) {
      Token dt_name = dt->name;
      int dot_idx = -1;
      for (unsigned int i = 0; i < dt_name.len; i++) {
        if (dt_name.start[i] == '.') {
          dot_idx = i;
          break;
        }
      }

      if (dot_idx != -1) {
        Token prefix = {dt_name.start, dot_idx, TOKEN_IDENTIF, 0, 0};
        Token suffix = {dt_name.start + dot_idx + 1, dt_name.len - dot_idx - 1,
                        TOKEN_IDENTIF, 0, 0};

        if (alias_map) {
          Token *real_mod = map_get(alias_map, prefix.start, prefix.len);
          if (real_mod)
            prefix = *real_mod;
        }

        size_t combined_len = prefix.len + 1 + suffix.len;
        char *combined = arena_alloc(arena, combined_len + 1);
        sprintf(combined, "%.*s_%.*s", (int)prefix.len, prefix.start,
                (int)suffix.len, suffix.start);
        dt_name.start = combined;
        dt_name.len = combined_len;
      }

      DepNode *dep = arena_alloc(arena, sizeof(DepNode));
      dep->name = dt_name;
      dep->next = meta->uses_types;
      meta->uses_types = dep;
    }
  }

  return VISIT_CONTINUE;
}

void extract_decl_dependencies(Arena *arena, AstNode *decl_root,
                               DeclMetadata *meta, HashMap *alias_map) {
  if (!decl_root)
    return;

  ExtDeclDepData data = {arena, meta, alias_map};
  AstVisitor visitor = {0};
  visitor.user_data = &data;
  visitor.enter_node = ext_decl_dep_enter;

  jmp_buf panic_env;
  visitor.panic_env = &panic_env;

  if (setjmp(panic_env) == 0) {
    ast_traverse(&visitor, decl_root);
  } else {
    fprintf(stderr, "OOM encountered during dependency extraction.\n");
  }
}

// Generates the Declaration metadata for an entire module
DeclMetadata *analyze_module_declarations(Arena *arena, AstNode *module_root) {
  if (!module_root || module_root->type != AST_PROGRAM)
    return NULL;

  HashMap alias_map;
  map_init(&alias_map, arena, 32);

  // Extract aliased module paths
  AstNode *stmt = module_root->as.block.first_stmt;
  while (stmt) {
    if (stmt->type == AST_USE) {
      Token path = stmt->as.use_stmt.path;
      if (path.len > 2) {
        const char *raw = path.start + 1;
        int raw_len = path.len - 2;
        const char *mod_name = raw;
        int mod_name_len = raw_len;
        for (int i = raw_len - 1; i >= 0; i--) {
          if (raw[i] == '/') {
            mod_name = raw + i + 1;
            mod_name_len = raw_len - (i + 1);
            break;
          }
        }
        Token alias = stmt->as.use_stmt.alias;
        Token *mod_tok = arena_alloc(arena, sizeof(Token));
        mod_tok->start = mod_name;
        mod_tok->len = mod_name_len;
        mod_tok->type = TOKEN_IDENTIF;

        if (alias.len > 0) {
          map_set(&alias_map, alias.start, alias.len, mod_tok);
        } else {
          map_set(&alias_map, mod_name, mod_name_len, mod_tok);
        }
      }
    }
    stmt = stmt->next;
  }

  DeclMetadata *head = NULL;
  DeclMetadata *tail = NULL;

  stmt = module_root->as.block.first_stmt;
  while (stmt) {
    Token name = {0};

    // Extract identifiers for top-level constructs
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
      DeclMetadata *meta = arena_alloc(arena, sizeof(DeclMetadata));
      meta->name = name;
      meta->type = stmt->type;
      meta->src_start = stmt->src_start;
      meta->src_end = stmt->src_end;

      meta->node_hash = compute_node_hash(stmt);

      meta->calls_to = NULL;
      meta->uses_types = NULL;
      meta->is_dirty = false;
      meta->next = NULL;

      extract_decl_dependencies(arena, stmt, meta, &alias_map);

      if (!head)
        head = tail = meta;
      else {
        tail->next = meta;
        tail = meta;
      }
    }
    stmt = stmt->next;
  }

  map_free_buckets(&alias_map);
  return head;
}

bool token_match(Token a, Token b) {
  if (a.len != b.len)
    return false;
  if (a.start == b.start)
    return true;
  if (!a.start || !b.start)
    return false;
  return memcmp(a.start, b.start, a.len) == 0;
}

DeclMetadata *find_decl_by_name(DeclMetadata *list, Token name) {
  for (DeclMetadata *curr = list; curr != NULL; curr = curr->next) {
    if (token_match(curr->name, name)) {
      return curr;
    }
  }
  return NULL;
}

void propagate_declaration_invalidation(DeclMetadata *old_cached_decls,
                                        DeclMetadata *new_parsed_decls) {
  if (!new_parsed_decls)
    return;

  size_t total_decls = 0;

  for (DeclMetadata *n = new_parsed_decls; n != NULL; n = n->next) {
    total_decls++;

    DeclMetadata *old = find_decl_by_name(old_cached_decls, n->name);
    if (!old) {
      // Its a completely new function/struct declaration
      n->is_dirty = true;
    } else if (old->node_hash != n->node_hash) {
      // The signature or internal body expression changed
      n->is_dirty = true;
    } else {
      // Unchanged on its own merits, but pending dependency checks
      n->is_dirty = false;
    }
  }

  DeclMetadata **worklist = malloc(sizeof(DeclMetadata *) * total_decls);
  size_t wl_top = 0;

  for (DeclMetadata *n = new_parsed_decls; n != NULL; n = n->next) {
    if (n->is_dirty) {
      worklist[wl_top++] = n;
    }
  }

  while (wl_top > 0) {
    DeclMetadata *dirty_item = worklist[--wl_top];

    // Scan all other declarations to see if they depend on this newly dirtied
    // item
    for (DeclMetadata *n = new_parsed_decls; n != NULL; n = n->next) {
      if (n->is_dirty)
        continue;

      bool structural_dependency_found = false;

      // Check if n calls the dirty function/method
      for (DepNode *dep = n->calls_to; dep != NULL; dep = dep->next) {
        if (token_match(dep->name, dirty_item->name)) {
          structural_dependency_found = true;
          break;
        }
      }

      // Check if n relies on the dirty type (struct, union, enum, etc.)
      if (!structural_dependency_found) {
        for (DepNode *dep = n->uses_types; dep != NULL; dep = dep->next) {
          if (token_match(dep->name, dirty_item->name)) {
            structural_dependency_found = true;
            break;
          }
        }
      }

      // If a dependency is spotted, mark it dirty and push to track its
      // downstream callers
      if (structural_dependency_found) {
        n->is_dirty = true;
        worklist[wl_top++] = n;
      }
    }
  }

  free(worklist);
}

void cache_write_decl_meta(const char *path, DeclMetadata *meta,
                           const char *src_base) {
  FILE *fp = fopen(path, "wb");
  if (!fp)
    return;

  uint32_t count = 0;
  for (DeclMetadata *m = meta; m; m = m->next)
    count++;
  fwrite(&count, sizeof(uint32_t), 1, fp);

  for (DeclMetadata *m = meta; m; m = m->next) {
    // Write Name & Hash
    fwrite(&m->name.len, sizeof(uint32_t), 1, fp);
    fwrite(m->name.start, 1, m->name.len, fp);
    fwrite(&m->node_hash, sizeof(uint32_t), 1, fp);

    uint32_t start_off = (m->src_start && m->src_start >= src_base)
                             ? (uint32_t)(m->src_start - src_base)
                             : 0xFFFFFFFF;
    uint32_t end_off = (m->src_end && m->src_end >= src_base)
                           ? (uint32_t)(m->src_end - src_base)
                           : 0xFFFFFFFF;
    fwrite(&start_off, sizeof(uint32_t), 1, fp);
    fwrite(&end_off, sizeof(uint32_t), 1, fp);

    // Write Calls Dependencies
    uint32_t dep_count = 0;
    for (DepNode *d = m->calls_to; d; d = d->next)
      dep_count++;
    fwrite(&dep_count, sizeof(uint32_t), 1, fp);
    for (DepNode *d = m->calls_to; d; d = d->next) {
      fwrite(&d->name.len, sizeof(uint32_t), 1, fp);
      fwrite(d->name.start, 1, d->name.len, fp);
    }

    // Write Type Dependencies
    dep_count = 0;
    for (DepNode *d = m->uses_types; d; d = d->next)
      dep_count++;
    fwrite(&dep_count, sizeof(uint32_t), 1, fp);
    for (DepNode *d = m->uses_types; d; d = d->next) {
      fwrite(&d->name.len, sizeof(uint32_t), 1, fp);
      fwrite(d->name.start, 1, d->name.len, fp);
    }
  }
  fclose(fp);
}

DeclMetadata *cache_read_decl_meta(Arena *arena, const char *path,
                                   const char *src_base) {
#define SAFE_FREAD(ptr, size, nmemb, fp)                                       \
  do {                                                                         \
    if (fread((ptr), (size), (nmemb), (fp)) != (nmemb)) {                      \
      fclose(fp);                                                              \
      return NULL;                                                             \
    }                                                                          \
  } while (0)
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  uint32_t count = 0;
  if (fread(&count, sizeof(uint32_t), 1, fp) != 1) {
    fclose(fp);
    return NULL;
  }

  DeclMetadata *head = NULL, *tail = NULL;
  for (uint32_t i = 0; i < count; i++) {
    DeclMetadata *m = arena_alloc(arena, sizeof(DeclMetadata));
    memset(m, 0, sizeof(DeclMetadata));

    // Read Name inline to survive source file modifications
    SAFE_FREAD(&m->name.len, sizeof(uint32_t), 1, fp);
    char *name_str = arena_alloc(arena, m->name.len);
    SAFE_FREAD(name_str, 1, m->name.len, fp);
    m->name.start = name_str;
    m->name.type = TOKEN_IDENTIF;

    SAFE_FREAD(&m->node_hash, sizeof(uint32_t), 1, fp);

    uint32_t start_off, end_off;
    SAFE_FREAD(&start_off, sizeof(uint32_t), 1, fp);
    SAFE_FREAD(&end_off, sizeof(uint32_t), 1, fp);
    m->src_start = (start_off == 0xFFFFFFFF) ? NULL : (src_base + start_off);
    m->src_end = (end_off == 0xFFFFFFFF) ? NULL : (src_base + end_off);

    uint32_t dep_count;

    // Read Calls
    SAFE_FREAD(&dep_count, sizeof(uint32_t), 1, fp);
    for (uint32_t j = 0; j < dep_count; j++) {
      DepNode *d = arena_alloc(arena, sizeof(DepNode));
      SAFE_FREAD(&d->name.len, sizeof(uint32_t), 1, fp);
      char *d_name = arena_alloc(arena, d->name.len);
      SAFE_FREAD(d_name, 1, d->name.len, fp);
      d->name.start = d_name;
      d->next = m->calls_to;
      m->calls_to = d;
    }

    // Read Types
    SAFE_FREAD(&dep_count, sizeof(uint32_t), 1, fp);
    for (uint32_t j = 0; j < dep_count; j++) {
      DepNode *d = arena_alloc(arena, sizeof(DepNode));
      SAFE_FREAD(&d->name.len, sizeof(uint32_t), 1, fp);
      char *d_name = arena_alloc(arena, d->name.len);
      SAFE_FREAD(d_name, 1, d->name.len, fp);
      d->name.start = d_name;
      d->next = m->uses_types;
      m->uses_types = d;
    }

    if (!head)
      head = tail = m;
    else {
      tail->next = m;
      tail = m;
    }
  }
  fclose(fp);
  return head;
}
