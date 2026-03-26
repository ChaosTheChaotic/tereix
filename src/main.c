#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define ARENA_CHUNK_SIZE (1024 * 1024)

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t capacity;
  size_t used;
  uint8_t data[];
} ArenaBlock;

typedef struct {
  ArenaBlock *current;
} Arena;

// Align the pointer to the nearest 8 bytes
static size_t align_size(size_t size) {
  return (size + 7) & ~7;
}

ArenaBlock* arena_new_block(size_t size) {
  size_t block_size = size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE;
  // Total size = Block metadata + the data buffer
  ArenaBlock *block = malloc(sizeof(ArenaBlock) + block_size);
  block->next = NULL;
  block->capacity = block_size;
  block->used = 0;
  return block;
}

void* arena_alloc(Arena *arena, size_t size) {
  size = align_size(size);

  // If no block exists or current block is full
  if (!arena->current || arena->current->used + size > arena->current->capacity) {
    ArenaBlock *new_block = arena_new_block(size);
    new_block->next = arena->current;
    arena->current = new_block;
  }

  void *ptr = &arena->current->data[arena->current->used];
  arena->current->used += size;
  return ptr;
}

void arena_free_all(Arena *arena) {
  ArenaBlock *curr = arena->current;
  while (curr) {
    ArenaBlock *next = curr->next;
    free(curr);
    curr = next;
  }
  arena->current = NULL;
}

#define SIZES(X) X(8) X(16) X(32) X(64) X(128) X(size)

#define AS_UNSIGNED(n) "u" #n ,
#define AS_SIGNED(n)   "i" #n ,
#define AS_FLOAT(n)    "f" #n ,

typedef enum {
  TOKEN_ASSIGN,
  TOKEN_OP,
  TOKEN_IDENTIF,
  TOKEN_LIT,
  TOKEN_KW,
  TOKEN_PUNC,
  TOKEN_COMPARE,
  TOKEN_EOF,
  TOKEN_UNKNOWN,
} TOKEN_TYPE;

typedef struct {
  char *start;
  unsigned int len;
  TOKEN_TYPE type;
} Token;

typedef struct {
  char *start;
  char *curr;
  unsigned int line;
  unsigned int col;
} LexCtx;

typedef struct {
  bool is_static;
  bool is_mut;
  bool is_custom;
  Token name;
  unsigned int array_dimens; // 0 for not an array 1 for [] etc.
} DataType;

typedef enum {
  AST_BINOP,
  AST_UOP,
  AST_IDENTIF,
  AST_VAR_DECL,
  AST_NUM_LIT,
  AST_STR_LIT,
  AST_CHAR_LIT,
  AST_IF,
  AST_BLOCK,
  AST_STRUCT,
  AST_UNION,
  AST_ENUM,
  AST_DEFER,
  AST_FOR,
  AST_WHILE,
  AST_FUNC,
  AST_PARAM,
} ASTN_TYPE;

typedef struct AstNode {
  ASTN_TYPE type;
  struct AstNode *next;
  union {
    struct { Token val; } num_lit;
    struct { Token val; } str_lit;
    struct { Token val; } char_lit;
    struct { Token val; } identif;
    struct {
      Token op;
      struct AstNode *left;
      struct AstNode *right;
    } binop;
    struct {
      Token op;
      struct AstNode *operand;
    } unop;
    struct {
      Token id;
      DataType type;
      struct AstNode *init; // Expr assigned
    } var_decl;
    struct {
      Token if_stmt;
      struct AstNode *check;
      struct AstNode *action;
    } if_check;
    struct {
      Token structn;
      struct AstNode *contents;
    } struct_def;
    struct {
      Token unionn;
      struct AstNode *contents;
    } union_def;
    struct {
      Token enumn;
      struct AstNode *contents;
    } enum_def;
    struct {
      Token defer;
      struct AstNode *contents;
    } defer_stmt;
    struct {
      Token for_stmt;
      struct AstNode *check;
      struct AstNode *action;
    } for_loop;
    struct {
      Token while_stmt;
      struct AstNode *check;
      struct AstNode *action;
    } while_loop;
    struct {
      DataType type;
      Token id;
    } fn_param;
    struct {
      Token fn_name;
      DataType ret_type;
      struct AstNode *params;
      struct AstNode *block;
    } func_def;
    struct { struct AstNode *first_stmt; } block;
  } as;
} AstNode;

AstNode* new_node(Arena *arena, ASTN_TYPE type) {
  AstNode *node = arena_alloc(arena, sizeof(AstNode));
  memset(node, 0, sizeof(AstNode)); // Clear memory
  node->type = type;
  return node;
}

typedef enum {
  STATE_GLOBAL,       // Looking for funcs, structs, global vars
  STATE_IN_FUNC,      // Looking for statements
  STATE_IN_EXPR,      // Currently parsing math/logic
  STATE_IN_STRUCT_DEF,
  STATE_IN_UNION_DEF,
  STATE_IN_ENUM_DEF,
  STATE_IN_IF_EXPECT_COND
} ParseState;

typedef struct {
  LexCtx *lex;
  Token curr;
  Token prev;
  Arena *arena;

  ParseState *state_stack;
  size_t state_count;
  size_t state_cap;

  AstNode **node_stack;
  size_t node_count;
  size_t node_cap;
} ParseCtx;

void push_state(ParseCtx *ctx, ParseState state) {
  if (ctx->state_count >= ctx->state_cap) {
    size_t old_cap = ctx->state_cap;
    ctx->state_cap = (old_cap == 0) ? 32 : old_cap * 2;

    ParseState *new_stack = arena_alloc(ctx->arena, sizeof(ParseState) * ctx->state_cap);

    if (ctx->state_stack) {
      memcpy(new_stack, ctx->state_stack, sizeof(ParseState) * old_cap);
    }
    ctx->state_stack = new_stack;
  }
  ctx->state_stack[ctx->state_count++] = state;
}

ParseState pop_state(ParseCtx *ctx) {
  if (ctx->state_count == 0) {
    fprintf(stderr, "Parser Error: State stack underflow\n");
    exit(1);
  }
  return ctx->state_stack[--ctx->state_count];
}

void push_node(ParseCtx *ctx, AstNode *node) {
  if (ctx->node_count >= ctx->node_cap) {
    size_t old_cap = ctx->node_cap;
    ctx->node_cap = (old_cap == 0) ? 32 : old_cap * 2;

    AstNode **new_stack = arena_alloc(ctx->arena, sizeof(AstNode*) * ctx->node_cap);

    if (ctx->node_stack) {
      memcpy(new_stack, ctx->node_stack, sizeof(AstNode*) * old_cap);
    }
    ctx->node_stack = new_stack;
  }
  ctx->node_stack[ctx->node_count++] = node;
}

AstNode* pop_node(ParseCtx *ctx) {
  if (ctx->node_count == 0) {
    fprintf(stderr, "Parser Error: Node stack underflow\n");
    exit(1);
  }
  return ctx->node_stack[--ctx->node_count];
}

bool check_exists(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "r")) != NULL) {
    fclose(fp);
    return true;
  } else {
    return false;
  }
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

inline void print_help() {
  printf("Literally just give it a valid file bro smh");
}

inline bool is_newline(char c) {
  return ( c == '\n' || c == '\r');
}

const char* load_file(const char *path) {
  FILE *fp = NULL;
  if ((fp = fopen(path, "rb")) != NULL) {
    if (fseek(fp, 0, SEEK_END) != 0) {
      perror("Error seeking to end of file");
      fclose(fp);
      return NULL;
    }

    long fsize;
    if ((fsize = ftell(fp)) == -1) {
      perror("Failed to get file size");
      fclose(fp);
      return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
      perror("Error seeking to start of file");
      fclose(fp);
      return NULL;
    }

    char *file = malloc(sizeof(char) * (fsize + 1));
    if (!file) {
      fprintf(stderr, "Failed to malloc the file");
      fclose(fp);
      return NULL;
    }

    unsigned long bin = fread(file, sizeof(char), fsize, fp);
    if (bin != (unsigned long)fsize) {
      fprintf(stderr, "Bytes read into buffer != the size of the file");
      free(file);
      fclose(fp);
      return NULL;
    }

    file[fsize] = '\0';
    fclose(fp);
    return file;
  } else {
    return NULL;
  }
}

const char *kwlist[] = {
  SIZES(AS_UNSIGNED)
  SIZES(AS_SIGNED)
  SIZES(AS_FLOAT)
  "mut",
  "bool",
  "str", // Technically should be parsed as char[] but oh well
  "void",
  "char",
  "auto",
  "any",
  "static",
  "if",
  "for",
  "while",
  "ret",
  "defer",
  "else",
  "struct",
  "union",
  "enum",
};
const size_t kwlistlen = sizeof(kwlist) / sizeof(kwlist[0]);

const char *oplist[] = {
  "^",
  "&",
  "|",
  "!",
  "<<",
  ">>",
  "+",
  "-",
  "/",
  "*",
};
const size_t oplistlen = sizeof(oplist) / sizeof(oplist[0]);

const char *complist[] = {
  "==", "!=", "<=", ">=", "<", ">"
};
const size_t complistlen = sizeof(complist) / sizeof(complist[0]);

inline bool is_kw(char *start, unsigned int len) {
  for (unsigned int i = 0; i < kwlistlen; i++) {
    if (strlen(kwlist[i]) == len && strncmp(start, kwlist[i], len) == 0) {
      return true;
    }
  }
  return false;
}

inline bool is_op(char *start, unsigned int len) {
  for (unsigned int i = 0; i < oplistlen; i++) {
    if (strlen(oplist[i]) == len && strncmp(start, oplist[i], len) == 0) {
      return true;
    }
  }
  return false;
}

inline bool is_punc(char c) {
  switch (c) {
    case ',': case '{': case '}': case '(': 
    case ')': case '[': case ']': case ';': case '.': return true;
    default: return false;
  }
}

inline bool is_numeric_slice(const char *start, unsigned int len) {
  if (len == 0) return false;

  unsigned int i = 0;
  bool has_decimal = false;
  bool has_digits = false;

  if (start[0] == '-') {
    i++;
  }

  for (; i < len; i++) {
    if (start[i] == '.') {
      if (has_decimal) return false;
      has_decimal = true;
    } else if (start[i] >= '0' && start[i] <= '9') {
      has_digits = true;
    } else {
      return false;
    }
  }

  return has_digits;
}

bool is_lit(const char *start, unsigned int len) {
  if (len == 0) return false;

  if (len >= 2 && ((start[0] == '"' && start[len - 1] == '"') || (start[0] == '\'' && start[len - 1] == '\'')) && start[len - 2] == '\\') {
    return true;
  }

  if ((len == 4 && strncmp(start, "true", 4) == 0) ||
    (len == 5 && strncmp(start, "false", 5) == 0)) {
    return true;
  }

  return is_numeric_slice(start, len);
}

inline bool is_compare(char *start, unsigned int len) {
  for (unsigned int i = 0; i < complistlen; i++) {
    if (strlen(complist[i]) == len && strncmp(start, complist[i], len) == 0) {
      return true;
    }
  }
  return false;
}

void skip_irrelevant(LexCtx *ctx) {
  while (*ctx->curr != '\0') {
    if (isspace(*ctx->curr)) {
      if (is_newline(*ctx->curr)) {
	ctx->line++;
	ctx->col = 0;
      } else {
	ctx->col++;
      }
      ctx->curr++;
    } else if (strncmp(ctx->curr, "//", 2) == 0) {
      while (*ctx->curr != '\0' && !is_newline(*ctx->curr)) {
	ctx->curr++;
	ctx->col++;
      }
    } else {
      break;
    }
  }
}

Token next_token(LexCtx *ctx) {
  skip_irrelevant(ctx);

  ctx->start = ctx->curr;
  if (*ctx->curr == '\0') {
    return (Token){ .start = ctx->start, .len = 0, .type = TOKEN_EOF };
  }

  TOKEN_TYPE type;
  unsigned int len = 0;

  // Identifiers, Keywords, Booleans
  if (isalpha(*ctx->curr) || *ctx->curr == '_') {
    while (isalnum(*ctx->curr) || *ctx->curr == '_') {
      ctx->curr++; ctx->col++;
    }
    len = ctx->curr - ctx->start;

    if ((len == 4 && strncmp(ctx->start, "true", 4) == 0) ||
      (len == 5 && strncmp(ctx->start, "false", 5) == 0)) {
      type = TOKEN_LIT;
    } 
    else if (is_kw(ctx->start, len)) {
      type = TOKEN_KW;
    } 
    else {
      type = TOKEN_IDENTIF;
    }
  } 

  // Numeric Literals
  else if (isdigit(*ctx->curr)) {
    bool has_dot = false;
    while (isdigit(*ctx->curr) || *ctx->curr == '.') {
      if (*ctx->curr == '.') {
	if (has_dot) break;
	has_dot = true;
      }
      ctx->curr++; ctx->col++;
    }
    len = ctx->curr - ctx->start;
    type = TOKEN_LIT;
  } 

  // String/Char Literals
  else if (*ctx->curr == '"' || *ctx->curr == '\'') {
    char quote = *ctx->curr;
    ctx->curr++;
    ctx->col++;

    while (*ctx->curr != '\0' && *ctx->curr != quote) {
      if (*ctx->curr == '\\') {
	ctx->curr++; // Skip '\'
	ctx->col++;
	char escape = *ctx->curr;
	switch (escape) {
	  case 'n': case 't': case 'r': case '\\': case '"': case '\'':
	    // Valid (not implementing unicode and all the other ones bro gimmie a break)
	    break;
	  default:
	    fprintf(stderr, "Error: Invalid escape sequence \\%c\n", escape);
	    return (Token){ .start = ctx->start, .len = (ctx->curr - ctx->start), .type = TOKEN_UNKNOWN };
	}
	if (*ctx->curr == '\0') break;
	ctx->curr++; 
	ctx->col++;
	continue; // Jump to start of loop to check for end of string
      }

      if (is_newline(*ctx->curr)) {
	ctx->line++;
	ctx->col = 0; 
      } else {
	ctx->col++;
      }
      ctx->curr++;
    }

    if (*ctx->curr == '\0') {
      printf("Error: Unterminated string at line %u col %u\n", ctx->line, ctx->col);
      return (Token){ .start = ctx->start, .len = (ctx->curr - ctx->start), .type = TOKEN_UNKNOWN };
    } else {
      if (quote == '\'' && (ctx->curr - ctx->start) > 3) {
	fprintf(stderr, "Char literal must contain only 1 char");
	return (Token){ .start = ctx->start, .len = (ctx->curr - ctx->start), .type = TOKEN_UNKNOWN};
      }
      ctx->curr++; 
      ctx->col++;
      len = ctx->curr - ctx->start;
      type = TOKEN_LIT;
    }
  }

  // Operators and Punctuation
  else {
    if (is_compare(ctx->curr, 2) || is_op(ctx->curr, 2)) {
      type = is_compare(ctx->curr, 2) ? TOKEN_COMPARE : TOKEN_OP;
      len = 2;
    } else if (*ctx->curr == '=') {
      type = TOKEN_ASSIGN;
      len = 1;
    } else if (is_compare(ctx->curr, 1) || is_op(ctx->curr, 1) || is_punc(*ctx->curr)) {
      len = 1;
      if (is_compare(ctx->curr, 1)) type = TOKEN_COMPARE;
      else if (is_op(ctx->curr, 1)) type = TOKEN_OP;
      else if (is_punc(*ctx->curr)) type = TOKEN_PUNC;
      else type = TOKEN_UNKNOWN;
    } else {
      type = TOKEN_UNKNOWN;
      len = 1;
    }

    ctx->curr += len;
    ctx->col += len;
  }

  return (Token){ .start = ctx->start, .len = len, .type = type };
}

Token peek_token(LexCtx *ctx) {
  char *saved_curr = ctx->curr;
  unsigned int saved_line = ctx->line;
  unsigned int saved_col = ctx->col;

  Token t = next_token(ctx);

  ctx->curr = saved_curr;
  ctx->line = saved_line;
  ctx->col = saved_col;

  return t;
}

void adv(ParseCtx *ctx) {
  ctx->prev = ctx->curr;
  ctx->curr = next_token(ctx->lex);
}

bool is_builtin_type_kw(Token t) {
  if (t.type != TOKEN_KW) return false;

  // Check macro int definitions
  for (unsigned int i = 0; i < 18; i++) {
    if (strlen(kwlist[i]) == t.len && strncmp(t.start, kwlist[i], t.len) == 0) {
      return true;
    }
  }

  // Check other types
  const char *extra_types[] = {"bool", "str", "void", "char", "auto", "any"};
  for (unsigned int i = 0; i < 6; i++) {
    if (strlen(extra_types[i]) == t.len && strncmp(t.start, extra_types[i], t.len) == 0) {
      return true;
    }
  }
  return false;
}

bool is_type(ParseCtx *ctx) {
  Token t = ctx->curr;

  if (t.type == TOKEN_KW) {
    if (strncmp(t.start, "static", t.len) == 0 || 
        strncmp(t.start, "mut", t.len) == 0) {
      return true;
    }
  }

  if (is_builtin_type_kw(t)) return true;

  // Might be a custom type
  if (t.type == TOKEN_IDENTIF) return true;

  return false;
}

DataType parse_type(ParseCtx *ctx) {
  DataType type = {0};

  while (ctx->curr.type == TOKEN_KW) {
    if (strncmp(ctx->curr.start, "static", ctx->curr.len) == 0) {
      type.is_static = true;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "mut", ctx->curr.len) == 0) {
      type.is_mut = true;
      adv(ctx);
    } else {
      break;
    }
  }

  if (ctx->curr.type == TOKEN_IDENTIF || is_builtin_type_kw(ctx->curr)) {
    type.name = ctx->curr;
    if (ctx->curr.type == TOKEN_IDENTIF) type.is_custom = true;
    adv(ctx);
  } else {
    fprintf(stderr, "Expected type name at line %u\n", ctx->lex->line);
  }

  while (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '[') {
    adv(ctx);
    if (*ctx->curr.start != ']') {
      fprintf(stderr, "Fixed size arrays not yet supported at line %u\n", ctx->lex->line);
    }
    adv(ctx);
    type.array_dimens++;
  }

  return type;
}

bool parse(ParseCtx *ctx) {
  push_state(ctx, STATE_GLOBAL);
  while (ctx->state_count > 0 && ctx->curr.type != TOKEN_EOF) {
    ParseState current_state = pop_state(ctx);

    switch (current_state) {
      case STATE_GLOBAL: {
	if (ctx->curr.type == TOKEN_KW) {
	  if (strncmp(ctx->curr.start, "struct", ctx->curr.len) == 0) {

	    adv(ctx);
	    AstNode *snode = new_node(ctx->arena, AST_STRUCT);
	    snode->as.struct_def.structn = ctx->curr;
	    push_node(ctx, snode);
	    // How handle contents?
	    push_state(ctx, STATE_GLOBAL);
	    push_state(ctx, STATE_IN_STRUCT_DEF);
	    adv(ctx);
	    break;
	  } else if (strncmp(ctx->curr.start, "union", ctx->curr.len) == 0) {
	    adv(ctx);
	    AstNode *unode = new_node(ctx->arena, AST_UNION);
	    unode->as.union_def.unionn = ctx->curr;
	    push_node(ctx, unode);
	    // How handle contents?
	    push_state(ctx, STATE_GLOBAL);
	    push_state(ctx, STATE_IN_UNION_DEF);
	    adv(ctx);
	    break;
	  } else if (strncmp(ctx->curr.start, "enum", ctx->curr.len) == 0) {
	    adv(ctx);
	    AstNode *enode = new_node(ctx->arena, AST_UNION);
	    enode->as.union_def.unionn = ctx->curr;
	    push_node(ctx, enode);
	    // How handle contents?
	    push_state(ctx, STATE_GLOBAL);
	    push_state(ctx, STATE_IN_ENUM_DEF);
	    adv(ctx);
	    break;
	  }
	}
	if (is_type(ctx)) {
	  DataType type = parse_type(ctx);

	  if (ctx->curr.type != TOKEN_IDENTIF) {
	    fprintf(stderr, "Expected identifier after type at line %u\n", ctx->lex->line);
	    return false;
	  }
	  Token name = ctx->curr;
	  adv(ctx);

	  if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
	    
	    AstNode *fnode = new_node(ctx->arena, AST_FUNC);
	    fnode->as.func_def.fn_name = name;
	    fnode->as.func_def.ret_type = type;
	    push_node(ctx, fnode);

	    push_state(ctx, STATE_GLOBAL);
	    push_state(ctx, STATE_IN_FUNC);
	    adv(ctx);
	    break;
	  } else {
	    // Construct VAR ast node and push
	    AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
	    vnode->as.var_decl.type = type;
	    vnode->as.var_decl.id = name;
	    // Var definitions could be if statement based etc.
	  }
	}
	fprintf(stderr, "Unexpected token in global scope at line %u\n", ctx->lex->line);
	break;
      }
      case STATE_IN_EXPR: {}
      case STATE_IN_FUNC: {}
      case STATE_IN_IF_EXPECT_COND: {}
      case STATE_IN_STRUCT_DEF: {}
      case STATE_IN_ENUM_DEF: {}
      case STATE_IN_UNION_DEF: {}
    }
  }
}

void try_compile(const char *path) {
  const char *file = load_file(path);
  if (!file) {
    fprintf(stderr, "Failed to load file: %s", path);
    exit(1);
  }
  LexCtx lex = {0};
  lex.start = (char *)file;
  lex.curr = (char *)file;
  lex.line = 1;
  lex.col = 1;

  Arena arena = {0};
  ParseCtx pctx = {0};
  pctx.lex = &lex;
  pctx.arena = &arena;
  pctx.curr = next_token(&lex);
  pctx.state_cap = 64;
  pctx.state_stack = arena_alloc(&arena, sizeof(ParseState) * pctx.state_cap);

  if (!parse(&pctx)) {
    fprintf(stderr, "Some error occurred during parsing");
    arena_free_all(&arena);
    free((void*)file);
    exit(127);
  }

  arena_free_all(&arena);
  free((void*)file);
  return;
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    print_help();
    exit(1);
  }

  bool found_file = false;
  for (unsigned i = 1; i < (unsigned int)argc; i++) {
    if (check_exists(argv[i])) {
      found_file = true;
      printf("Compiling %s\n", argv[i]);
      try_compile(argv[i]);
    }
  }

  if (!found_file) {
    printf("No valid file found\n");
    print_help();
    return 1;
  }

  return 0;
}
