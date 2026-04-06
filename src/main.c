#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static size_t align_size(size_t size) { return (size + 7) & ~7; }

ArenaBlock *arena_new_block(size_t size) {
  size_t block_size = size > ARENA_CHUNK_SIZE ? size : ARENA_CHUNK_SIZE;
  // Total size = Block metadata + the data buffer
  ArenaBlock *block = malloc(sizeof(ArenaBlock) + block_size);
  block->next = NULL;
  block->capacity = block_size;
  block->used = 0;
  return block;
}

void *arena_alloc(Arena *arena, size_t size) {
  size = align_size(size);

  // If no block exists or current block is full
  if (!arena->current ||
      arena->current->used + size > arena->current->capacity) {
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

#define AS_UNSIGNED(n) "u" #n,
#define AS_SIGNED(n) "i" #n,
#define AS_FLOAT(n) "f" #n,

typedef enum {
  TOKEN_ASSIGN,
  TOKEN_OP,
  TOKEN_IDENTIF,
  TOKEN_NUM_LIT,
  TOKEN_STR_LIT,
  TOKEN_CHAR_LIT,
  TOKEN_BOOL_LIT,
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

struct AstNode;

typedef struct {
  bool is_static;
  bool is_mut;
  bool is_custom;
	bool is_async;
	bool is_threadlocal;
	bool is_inline;
	unsigned int ptr_depth; // 2 for **type etc
	bool is_reference; // true with &type
  Token name;
  unsigned int array_dimens;  // 0 for not an array 1 for [] etc.
  struct AstNode **dim_sizes; // Per dimensions [][][] etc.
} DataType;

typedef enum {
  AST_BINOP,
  AST_UOP,
	AST_ADDR_OF,
	AST_DEREF,
  AST_IDENTIF,
  AST_VAR_DECL,
  AST_NUM_LIT,
  AST_STR_LIT,
  AST_CHAR_LIT,
  AST_BOOL_LIT,
  AST_ARRAY_LIT,
  AST_IF,
  AST_BLOCK,
  AST_STRUCT,
  AST_UNION,
  AST_ENUM,
  AST_ENUM_MEMBER,
  AST_DEFER,
  AST_FOR,
  AST_WHILE,
  AST_FUNC,
  AST_FUNC_CALL,
  AST_PARAM,
  AST_RET,
  AST_INDEX,
  AST_MEMBER,
  AST_PROGRAM, // The root node
} ASTN_TYPE;

typedef struct AstNode {
  ASTN_TYPE type;
  struct AstNode *next;
  union {
    struct {
      Token val;
    } num_lit;
    struct {
      Token val;
    } str_lit;
    struct {
      Token val;
    } char_lit;
    struct {
      Token val;
    } bool_lit;
		struct {
			struct AstNode *elements;
		} array_lit;
    struct {
      Token val;
    } identif;
    struct {
      Token op;
      struct AstNode *left;
      struct AstNode *right;
    } binop;
    struct {
      Token op;
      struct AstNode *operand;
			bool is_postfix;
    } unop;
    struct {
      Token id;
      DataType type;
      struct AstNode *init;
    } var_decl;
    struct {
      Token if_stmt;
      struct AstNode *check;
      struct AstNode *action;
      struct AstNode *elseAct; // else {} or else if () {}
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
      Token name;
      struct AstNode *val;
    } enum_member;
    struct {
      Token defer;
      struct AstNode *contents;
    } defer_stmt;
    struct {
      Token for_stmt;
      struct AstNode *init;
      struct AstNode *check;
      struct AstNode *inc;
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
      bool is_async;
			bool is_inline;
      struct AstNode *params;
      struct AstNode *block;
    } func_def;
    struct {
      Token ret_kw;
      struct AstNode *expr;
    } ret_stmt;
    struct {
      struct AstNode *first_stmt;
			bool is_async;
    } block;
    struct {
      struct AstNode *caller;
      struct AstNode *args;
    } func_call;
    struct {
      struct AstNode *base;
      struct AstNode *index;
    } index;
    struct {
      struct AstNode *base;
      Token name;
    } member;
  } as;
} AstNode;

AstNode *new_node(Arena *arena, ASTN_TYPE type) {
  AstNode *node = arena_alloc(arena, sizeof(AstNode));
  memset(node, 0, sizeof(AstNode));
  node->type = type;
  return node;
}

typedef enum {
  STATE_GLOBAL, // Looking for funcs, structs, global vars
  STATE_IN_FUNC,
  STATE_IN_EXPR,
	STATE_INDEX_DONE,
	STATE_IN_ARRAY_LIT,
	STATE_ARRAY_ELEMENT_DONE,
	STATE_EXPR_STMT_DONE,
  STATE_IN_STRUCT_DEF,
  STATE_IN_UNION_DEF,
  STATE_IN_ENUM_DEF,
  STATE_ENUM_MEMBER_DONE,
  STATE_IN_IF_EXPECT_COND,
  STATE_IF_COND_DONE,
  STATE_IF_BODY_DONE,
  STATE_ELSE_BODY_DONE,
  STATE_PARSE_BLOCK, // Reusable state to parse { ... }
	STATE_BLOCK_DONE,
	STATE_BLOCK_EXPR_DONE,
  STATE_WHILE_COND_DONE,
  STATE_WHILE_BODY_DONE,
  STATE_FOR_INC_DONE,
  STATE_FOR_INIT_DONE,
  STATE_FOR_COND_DONE,
  STATE_FOR_BODY_DONE,
  STATE_FOR_INIT_START,
  STATE_FOR_INIT_DECL_DONE,
  STATE_IN_FUNC_ARGS,
  STATE_ARG_DONE,
  STATE_DEFER_DONE,
  STATE_RET_DONE,
  STATE_VAR_INIT_DONE,
} ParseState;

typedef struct {
  Token op;
  bool is_unary;
  bool is_postfix;
} OpInfo;

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

  OpInfo *op_stack;
  size_t op_count;
  size_t op_cap;

	bool expect_operand;
} ParseCtx;

void push_state(ParseCtx *ctx, ParseState state) {
  if (ctx->state_count >= ctx->state_cap) {
    size_t new_cap = (ctx->state_cap == 0) ? 32 : ctx->state_cap * 2;

    ParseState *new_stack =
        realloc(ctx->state_stack, sizeof(ParseState) * new_cap);

    if (!new_stack) {
      fprintf(stderr, "Out of memory: state_stack\n");
      exit(1);
    }

    ctx->state_stack = new_stack;
    ctx->state_cap = new_cap;
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
    size_t new_cap = (ctx->node_cap == 0) ? 32 : ctx->node_cap * 2;
    AstNode **new_stack = realloc(ctx->node_stack, sizeof(AstNode *) * new_cap);
    if (!new_stack) {
      exit(1);
    }
    ctx->node_stack = new_stack;
    ctx->node_cap = new_cap;
  }
  ctx->node_stack[ctx->node_count++] = node;
}

AstNode *pop_node(ParseCtx *ctx) {
  if (ctx->node_count == 0) {
    fprintf(stderr, "Parser Error: Node stack underflow\n");
    exit(1);
  }
  return ctx->node_stack[--ctx->node_count];
}

void push_op(ParseCtx *ctx, Token op, bool is_unary, bool is_postfix) {
  if (ctx->op_count >= ctx->op_cap) {
    size_t new_cap = (ctx->op_cap == 0) ? 32 : ctx->op_cap * 2;
    OpInfo *new_stack = realloc(ctx->op_stack, sizeof(OpInfo) * new_cap);
    if (!new_stack)
      exit(1);
    ctx->op_stack = new_stack;
    ctx->op_cap = new_cap;
  }
  ctx->op_stack[ctx->op_count++] = (OpInfo){op, is_unary, is_postfix};
}

void apply_op(ParseCtx *ctx) {
  if (ctx->op_count == 0)
    return;

  OpInfo info = ctx->op_stack[--ctx->op_count];

  if (info.is_unary) {
    if (ctx->node_count < 1)
      return;
    AstNode *operand = pop_node(ctx);

    ASTN_TYPE type = AST_UOP;

    // Specialization logic
    if (info.op.len == 1) {
      if (*info.op.start == '&')
        type = AST_ADDR_OF;
      else if (*info.op.start == '*')
        type = AST_DEREF;
    }

    AstNode *unop = new_node(ctx->arena, type);
    unop->as.unop.op = info.op;
    unop->as.unop.operand = operand;
    unop->as.unop.is_postfix = info.is_postfix;

    push_node(ctx, unop);
  } else {
    if (ctx->node_count < 2)
      return;
    AstNode *right = pop_node(ctx);
    AstNode *left = pop_node(ctx);

    AstNode *binop = new_node(ctx->arena, AST_BINOP);
    binop->as.binop.op = info.op;
    binop->as.binop.left = left;
    binop->as.binop.right = right;

    push_node(ctx, binop);
  }
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

inline bool is_newline(char c) { return (c == '\n' || c == '\r'); }

const char *load_file(const char *path) {
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
    SIZES(AS_UNSIGNED) SIZES(AS_SIGNED) SIZES(AS_FLOAT) "mut",
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
		"async",
		"threadlocal",
		"inline",
};
const size_t kwlistlen = sizeof(kwlist) / sizeof(kwlist[0]);

const char *oplist[] = {
    "^", "&",  "|",  "!",  "<<", ">>", "+",  "-",  "/",  "*",
    "%", "+=", "-=", "/=", "*=", "%=", "^=", "++", "--",
};
const size_t oplistlen = sizeof(oplist) / sizeof(oplist[0]);

const char *complist[] = {"==", "!=", "<=", ">=", "<", ">"};
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
  case ',':
  case '{':
  case '}':
  case '(':
  case ')':
  case '[':
  case ']':
  case ';':
  case '.':
    return true;
  default:
    return false;
  }
}

inline bool is_numeric_slice(const char *start, unsigned int len) {
  if (len == 0)
    return false;

  unsigned int i = 0;
  bool has_decimal = false;
  bool has_digits = false;

  if (start[0] == '-') {
    i++;
  }

  for (; i < len; i++) {
    if (start[i] == '.') {
      if (has_decimal)
        return false;
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
  if (len == 0)
    return false;

  if (len >= 2 &&
      ((start[0] == '"' && start[len - 1] == '"') ||
       (start[0] == '\'' && start[len - 1] == '\'')) &&
      start[len - 2] == '\\') {
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
    return (Token){.start = ctx->start, .len = 0, .type = TOKEN_EOF};
  }

  TOKEN_TYPE type;
  unsigned int len = 0;

  // Identifiers, Keywords, Booleans
  if (isalpha(*ctx->curr) || *ctx->curr == '_') {
    while (isalnum(*ctx->curr) || *ctx->curr == '_') {
      ctx->curr++;
      ctx->col++;
    }
    len = ctx->curr - ctx->start;

    if ((len == 4 && strncmp(ctx->start, "true", 4) == 0) ||
        (len == 5 && strncmp(ctx->start, "false", 5) == 0)) {
      type = TOKEN_BOOL_LIT;
    } else if (is_kw(ctx->start, len)) {
      type = TOKEN_KW;
    } else {
      type = TOKEN_IDENTIF;
    }
  }

  // Numeric Literals
  else if (isdigit(*ctx->curr)) {
    bool has_dot = false;
    while (isdigit(*ctx->curr) || *ctx->curr == '.') {
      if (*ctx->curr == '.') {
        if (has_dot)
          break;
        has_dot = true;
      }
      ctx->curr++;
      ctx->col++;
    }
    len = ctx->curr - ctx->start;
    type = TOKEN_NUM_LIT;
  }

  // String/Char Literals
  else if (*ctx->curr == '"' || *ctx->curr == '\'') {
    char quote = *ctx->curr;
    ctx->curr++;
    ctx->col++;

    while (*ctx->curr != '\0' && *ctx->curr != quote) {
      if (*ctx->curr == '\\') {
        ctx->curr++;
        ctx->col++;
        char escape = *ctx->curr;
        switch (escape) {
        case 'n':
        case 't':
        case 'r':
        case '\\':
        case '"':
        case '\'':
          // Valid (not implementing unicode and all the other ones bro gimmie a
          // break)
          break;
        default:
          fprintf(stderr, "Error: Invalid escape sequence \\%c\n", escape);
          return (Token){.start = ctx->start,
                         .len = (ctx->curr - ctx->start),
                         .type = TOKEN_UNKNOWN};
        }
        if (*ctx->curr == '\0')
          break;
        ctx->curr++;
        ctx->col++;
        continue;
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
      printf("Error: Unterminated string at line %u col %u\n", ctx->line,
             ctx->col);
      return (Token){.start = ctx->start,
                     .len = (ctx->curr - ctx->start),
                     .type = TOKEN_UNKNOWN};
    } else {
      if (quote == '\'' &&
          ((ctx->curr - ctx->start) > 3 ||
           (*ctx->curr == '\\' && (ctx->curr - ctx->start > 4)))) {
        fprintf(stderr, "Char literal must contain only 1 char");
        return (Token){.start = ctx->start,
                       .len = (ctx->curr - ctx->start),
                       .type = TOKEN_UNKNOWN};
      }
      ctx->curr++;
      ctx->col++;
      len = ctx->curr - ctx->start;
      type = (quote == '"') ? TOKEN_STR_LIT : TOKEN_CHAR_LIT;
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
    } else if (is_compare(ctx->curr, 1) || is_op(ctx->curr, 1) ||
               is_punc(*ctx->curr)) {
      len = 1;
      if (is_compare(ctx->curr, 1))
        type = TOKEN_COMPARE;
      else if (is_op(ctx->curr, 1))
        type = TOKEN_OP;
      else if (is_punc(*ctx->curr))
        type = TOKEN_PUNC;
      else
        type = TOKEN_UNKNOWN;
    } else {
      type = TOKEN_UNKNOWN;
      len = 1;
    }

    ctx->curr += len;
    ctx->col += len;
  }

  return (Token){.start = ctx->start, .len = len, .type = type};
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
  if (t.type != TOKEN_KW)
    return false;
  for (unsigned int i = 0; i < kwlistlen; i++) {
    if (strlen(kwlist[i]) == t.len && strncmp(t.start, kwlist[i], t.len) == 0) {
      return true;
    }
  }
  return false;
}

bool is_type(ParseCtx *ctx) {
  Token t = ctx->curr;
  
  // Skip over any pointers or references
  while (t.type == TOKEN_OP && (*t.start == '*' || *t.start == '&')) {
      t = peek_token(ctx->lex);
  }

  if (t.type == TOKEN_KW) {
    if (strncmp(t.start, "static", t.len) == 0 ||
        strncmp(t.start, "mut", t.len) == 0) {
      return true;
    }
  }

  if (is_builtin_type_kw(t))
    return true;

	// Might be a custom type
  if (t.type == TOKEN_IDENTIF && peek_token(ctx->lex).type == TOKEN_IDENTIF)
    return true;

  return false;
}

int get_precedence(Token op, bool is_unary, bool is_postfix) {
  if (is_postfix)
    return 11;
  if (is_unary)
    return 10;

  if (op.type == TOKEN_ASSIGN)
    return 1;

  if (op.len == 1) {
    switch (*op.start) {
    case '*':
    case '/':
    case '%':
      return 8;
    case '+':
    case '-':
      return 7;
    case '<':
    case '>':
      return 5;
    case '&':
      return 4;
    case '^':
      return 3;
    case '|':
      return 2;
    }
  } else if (op.len == 2) {
    if (strncmp(op.start, "<<", 2) == 0 || strncmp(op.start, ">>", 2) == 0)
      return 6;
    if (strncmp(op.start, "<=", 2) == 0 || strncmp(op.start, ">=", 2) == 0)
      return 5;
    if (strncmp(op.start, "==", 2) == 0 || strncmp(op.start, "!=", 2) == 0)
      return 4;

    bool assign_matches = false;
    switch (*op.start) {
    case '*':
    case '/':
    case '%':
    case '+':
    case '-':
    case '^':
      assign_matches = true;
    }
    if (assign_matches && op.start[1] == '=')
      return 1; // += etc
  }
  return 0;
}

bool parse_step(ParseCtx *ctx);

DataType parse_type(ParseCtx *ctx) {
  DataType type = {0};

  while (ctx->curr.type == TOKEN_OP) {
    if (strncmp(ctx->curr.start, "*", 1) == 0) {
      type.ptr_depth++;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "&", 1) == 0) {
      if (type.is_reference) {
        fprintf(stderr,
                "Error: Cannot have multiple references '&&' at line %u\n",
                ctx->lex->line);
      }
      type.is_reference = true;
      adv(ctx);
    } else {
      break;
    }
  }

  while (ctx->curr.type == TOKEN_KW) {
    if (strncmp(ctx->curr.start, "static", ctx->curr.len) == 0) {
      type.is_static = true;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "mut", ctx->curr.len) == 0) {
      type.is_mut = true;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "async", ctx->curr.len) == 0) {
      type.is_async = true;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "inline", ctx->curr.len) == 0) {
      type.is_inline = true;
      adv(ctx);
    } else if (strncmp(ctx->curr.start, "threadlocal", ctx->curr.len) == 0) {
      type.is_threadlocal = true;
      adv(ctx);
    } else {
      break;
    }
  }

  if (ctx->curr.type == TOKEN_IDENTIF || is_builtin_type_kw(ctx->curr)) {
    type.name = ctx->curr;
    if (ctx->curr.type == TOKEN_IDENTIF)
      type.is_custom = true;
    adv(ctx);
  } else {
    fprintf(stderr, "Expected type name at line %u\n", ctx->lex->line);
  }

  while (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '[') {
    adv(ctx);
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      type.array_dimens++;
      continue;
    }

    ctx->expect_operand = true;
    push_state(ctx, STATE_IN_EXPR);

    size_t target_state = ctx->state_count - 1;
    while (ctx->state_count > target_state && ctx->curr.type != TOKEN_EOF) {
      if (!parse_step(ctx))
        return type;
    }

    AstNode *expr_node = pop_node(ctx);

    if (type.dim_sizes == NULL) {
      type.dim_sizes =
          arena_alloc(ctx->arena, sizeof(AstNode *) * 8);
    }

    type.dim_sizes[type.array_dimens] = expr_node;
    type.array_dimens++;
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ']' after array dimension at line %u\n",
              ctx->lex->line);
    }
  }

  return type;
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

inline bool is_lit_type(TOKEN_TYPE t) {
  return (t == TOKEN_NUM_LIT || t == TOKEN_STR_LIT || t == TOKEN_BOOL_LIT ||
          t == TOKEN_CHAR_LIT);
}

typedef struct {
  AstNode *node;
  int depth;
  const char *label;
} AstPrintItem;

void print_type_info(DataType type) {
  if (type.is_reference)
    printf("&");
  for (unsigned int i = 0; i < type.ptr_depth; i++) {
    printf("*");
  }

  if (type.is_static)
    printf("static ");
  if (type.is_mut)
    printf("mut ");
  if (type.is_threadlocal)
    printf("threadlocal ");

  printf("%.*s", type.name.len, type.name.start);

  for (unsigned int i = 0; i < type.array_dimens; i++) {
    printf("[]");
  }
}

void print_ast(AstNode *root) {
  if (!root)
    return;

  size_t stack_cap = 1024;
  AstPrintItem *stack = malloc(sizeof(AstPrintItem) * stack_cap);
  size_t top = 0;

  stack[top++] = (AstPrintItem){root, 0, "Root"};

  while (top > 0) {
    AstPrintItem item = stack[--top];
    AstNode *node = item.node;

    if (!node)
      continue;

    // Print indentation
    for (int i = 0; i < item.depth; i++) {
      printf("  | ");
    }

    printf("[%s] ", item.label);

    if (node->next) {
      if (top >= stack_cap - 5) {
        stack_cap *= 2;
        stack = realloc(stack, sizeof(AstPrintItem) * stack_cap);
      }
      stack[top++] = (AstPrintItem){node->next, item.depth, item.label};
    }

    int next_depth = item.depth + 1;

    switch (node->type) {
    case AST_PROGRAM:
      printf("PROGRAM\n");
      if (node->as.block.first_stmt) {
        stack[top++] =
            (AstPrintItem){node->as.block.first_stmt, next_depth, "Decl"};
      }
      break;

    case AST_FUNC:
      if (node->as.func_def.is_async) {
        printf("ASYNC ");
      }
      if (node->as.func_def.is_inline) {
        printf("INLINE ");
      }
      printf("FUNC (Return: ");
      print_type_info(node->as.func_def.ret_type);
      printf("): %.*s\n", node->as.func_def.fn_name.len,
             node->as.func_def.fn_name.start);

      stack[top++] =
          (AstPrintItem){node->as.func_def.block, next_depth, "Body"};
      if (node->as.func_def.params) {
        stack[top++] =
            (AstPrintItem){node->as.func_def.params, next_depth, "Param"};
      }
      break;

    case AST_PARAM:
      printf("PARAM (");
      print_type_info(node->as.fn_param.type);
      printf("): %.*s\n", node->as.fn_param.id.len, node->as.fn_param.id.start);
      break;

    case AST_VAR_DECL:
      printf("VAR_DECL (");
      print_type_info(node->as.var_decl.type);
      printf("): %.*s\n", node->as.var_decl.id.len, node->as.var_decl.id.start);

      if (node->as.var_decl.init) {
        stack[top++] =
            (AstPrintItem){node->as.var_decl.init, next_depth, "Init"};
      }
      break;

    case AST_BINOP:
      printf("BINOP (%.*s)\n", node->as.binop.op.len, node->as.binop.op.start);
      stack[top++] = (AstPrintItem){node->as.binop.right, next_depth, "Right"};
      stack[top++] = (AstPrintItem){node->as.binop.left, next_depth, "Left"};
      break;

    case AST_UOP:
      if (node->as.unop.is_postfix) {
        printf("POSTFIX_UOP (%.*s)\n", node->as.unop.op.len,
               node->as.unop.op.start);
      } else {
        printf("PREFIX_UOP (%.*s)\n", node->as.unop.op.len,
               node->as.unop.op.start);
      }
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    case AST_NUM_LIT:
      printf("NUM_LIT: %.*s\n", node->as.num_lit.val.len,
             node->as.num_lit.val.start);
      break;

    case AST_CHAR_LIT:
      printf("CHAR_LIT: %.*s\n", node->as.char_lit.val.len,
             node->as.char_lit.val.start);
      break;

    case AST_IDENTIF:
      printf("IDENTIF: %.*s\n", node->as.identif.val.len,
             node->as.identif.val.start);
      break;

    case AST_BLOCK:
      if (node->as.block.is_async) {
        printf("ASYNC ");
      }
      printf("BLOCK\n");
      if (node->as.block.first_stmt) {
        stack[top++] =
            (AstPrintItem){node->as.block.first_stmt, next_depth, "Stmt"};
      }
      break;

    case AST_IF:
      printf("IF_STMT\n");
      if (node->as.if_check.elseAct)
        stack[top++] =
            (AstPrintItem){node->as.if_check.elseAct, next_depth, "Else"};
      if (node->as.if_check.action)
        stack[top++] =
            (AstPrintItem){node->as.if_check.action, next_depth, "Then"};
      if (node->as.if_check.check)
        stack[top++] =
            (AstPrintItem){node->as.if_check.check, next_depth, "Cond"};
      break;

    case AST_RET:
      printf("RETURN\n");
      if (node->as.ret_stmt.expr)
        stack[top++] =
            (AstPrintItem){node->as.ret_stmt.expr, next_depth, "Expr"};
      break;

    case AST_FOR:
      printf("FOR_LOOP\n");
      if (node->as.for_loop.action)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.action, next_depth, "Body"};
      if (node->as.for_loop.inc)
        stack[top++] = (AstPrintItem){node->as.for_loop.inc, next_depth, "Inc"};
      if (node->as.for_loop.check)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.check, next_depth, "Cond"};
      if (node->as.for_loop.init)
        stack[top++] =
            (AstPrintItem){node->as.for_loop.init, next_depth, "Init"};
      break;

    case AST_FUNC_CALL:
      printf("FUNC_CALL\n");
      if (node->as.func_call.args)
        stack[top++] =
            (AstPrintItem){node->as.func_call.args, next_depth, "Args"};
      if (node->as.func_call.caller)
        stack[top++] =
            (AstPrintItem){node->as.func_call.caller, next_depth, "Caller"};
      break;

    case AST_WHILE:
      printf("WHILE_LOOP\n");
      if (node->as.while_loop.action)
        stack[top++] =
            (AstPrintItem){node->as.while_loop.action, next_depth, "Body"};
      if (node->as.while_loop.check)
        stack[top++] =
            (AstPrintItem){node->as.while_loop.check, next_depth, "Cond"};
      break;

    case AST_BOOL_LIT:
      printf("BOOL_LIT: %.*s\n", node->as.bool_lit.val.len,
             node->as.bool_lit.val.start);
      break;

    case AST_ENUM:
      printf("ENUM: %.*s\n", node->as.enum_def.enumn.len,
             node->as.enum_def.enumn.start);
      if (node->as.enum_def.contents) {
        stack[top++] =
            (AstPrintItem){node->as.enum_def.contents, next_depth, "Members"};
      }
      break;

    case AST_ENUM_MEMBER:
      printf("ENUM_MEMBER: %.*s\n", node->as.enum_member.name.len,
             node->as.enum_member.name.start);
      if (node->as.enum_member.val) {
        stack[top++] =
            (AstPrintItem){node->as.enum_member.val, next_depth, "Value"};
      }
      break;

    case AST_ARRAY_LIT:
      printf("ARRAY_LIT\n");
      if (node->as.array_lit.elements) {
        stack[top++] =
            (AstPrintItem){node->as.array_lit.elements, next_depth, "Elem"};
      }
      break;

    case AST_INDEX:
      printf("INDEX_ACCESS\n");
      stack[top++] = (AstPrintItem){node->as.index.index, next_depth, "Index"};
      stack[top++] = (AstPrintItem){node->as.index.base, next_depth, "Base"};
      break;

    case AST_MEMBER:
      printf("MEMBER_ACCESS: .%.*s\n", node->as.member.name.len,
             node->as.member.name.start);
      stack[top++] = (AstPrintItem){node->as.member.base, next_depth, "Base"};
      break;

    case AST_DEFER:
      printf("DEFER\n");
      if (node->as.defer_stmt.contents)
        stack[top++] =
            (AstPrintItem){node->as.defer_stmt.contents, next_depth, "Action"};
      break;

    case AST_STRUCT:
      printf("STRUCT: %.*s\n", node->as.struct_def.structn.len,
             node->as.struct_def.structn.start);
      if (node->as.struct_def.contents)
        stack[top++] =
            (AstPrintItem){node->as.struct_def.contents, next_depth, "Fields"};
      break;

    case AST_UNION:
      printf("UNION: %.*s\n", node->as.union_def.unionn.len,
             node->as.union_def.unionn.start);
      if (node->as.union_def.contents)
        stack[top++] =
            (AstPrintItem){node->as.union_def.contents, next_depth, "Fields"};
      break;

    case AST_ADDR_OF:
      printf("ADDRESS_OF\n");
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    case AST_DEREF:
      printf("DEREF\n");
      stack[top++] =
          (AstPrintItem){node->as.unop.operand, next_depth, "Operand"};
      break;

    default:
      printf("AST_NODE_TYPE_%d\n", node->type);
      break;
    }
  }

  free(stack);
  printf("\n");
}

bool parse_step(ParseCtx *ctx) {
  ParseState current_state = pop_state(ctx);

  switch (current_state) {
  case STATE_GLOBAL: {
    AstNode *root = ctx->node_stack[0];
    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "struct", ctx->curr.len) == 0) {
        adv(ctx);
        AstNode *snode = new_node(ctx->arena, AST_STRUCT);
        snode->as.struct_def.structn = ctx->curr;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '{' after struct name at line %u\n",
                  ctx->lex->line);
          return false;
        }
        push_node(ctx, snode);
        append_stmt(&root->as.block.first_stmt, snode);
        push_state(ctx, STATE_GLOBAL);
        push_state(ctx, STATE_IN_STRUCT_DEF);
        break;
      } else if (strncmp(ctx->curr.start, "union", ctx->curr.len) == 0) {
        adv(ctx);
        AstNode *unode = new_node(ctx->arena, AST_UNION);
        unode->as.union_def.unionn = ctx->curr;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '{' after union name at line %u\n",
                  ctx->lex->line);
          return false;
        }
        push_node(ctx, unode);
        append_stmt(&root->as.block.first_stmt, unode);
        push_state(ctx, STATE_GLOBAL);
        push_state(ctx, STATE_IN_UNION_DEF);
        break;
      } else if (strncmp(ctx->curr.start, "enum", ctx->curr.len) == 0) {
        adv(ctx);
        AstNode *enode = new_node(ctx->arena, AST_ENUM);
        enode->as.enum_def.enumn = ctx->curr;
        adv(ctx);
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '{' after struct enum at line %u\n",
                  ctx->lex->line);
          return false;
        }
        push_node(ctx, enode);
        append_stmt(&root->as.block.first_stmt, enode);
        push_state(ctx, STATE_GLOBAL);
        push_state(ctx, STATE_IN_ENUM_DEF);
        break;
      }
    }
    if (is_type(ctx)) {
      DataType type = parse_type(ctx);

      if (ctx->curr.type != TOKEN_IDENTIF) {
        fprintf(stderr, "Expected identifier after type at line %u\n",
                ctx->lex->line);
        return false;
      }
      Token name = ctx->curr;
      adv(ctx);

      if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
        if (type.is_threadlocal) {
          fprintf(stderr,
                  "Error: 'threadlocal' cannot be applied to function '%.*s' "
                  "at line %u\n",
                  name.len, name.start, ctx->lex->line);
          return false;
        }
        AstNode *fnode = new_node(ctx->arena, AST_FUNC);
        fnode->as.func_def.fn_name = name;
        fnode->as.func_def.ret_type = type;
				fnode->as.func_def.is_async = type.is_async;
				fnode->as.func_def.is_inline = type.is_inline;
        adv(ctx);

        AstNode *params_head = NULL;
        AstNode *params_tail = NULL;
        while (ctx->curr.type != TOKEN_EOF &&
               !(ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')')) {
          if (!is_type(ctx)) {
            fprintf(stderr,
                    "Expected type in function parameters at line %u, col %u\n",
                    ctx->lex->line, ctx->lex->col);
            return false;
          }
          DataType p_type = parse_type(ctx);
          if (ctx->curr.type != TOKEN_IDENTIF) {
            fprintf(stderr,
                    "Expected identifier after type in params at line %u\n",
                    ctx->lex->line);
            return false;
          }
          if (p_type.is_async) {
            fprintf(
                stderr,
                "Error: 'async' cannot be applied to parameter at line %u\n",
                ctx->lex->line);
            return false;
          }
          if (p_type.is_inline) {
            fprintf(
                stderr,
                "Error: 'inline' cannot be applied to parameter at line %u\n",
                ctx->lex->line);
            return false;
          }
          Token p_name = ctx->curr;
          adv(ctx);

          AstNode *pnode = new_node(ctx->arena, AST_PARAM);
          pnode->as.fn_param.type = p_type;
          pnode->as.fn_param.id = p_name;

          if (!params_head)
            params_head = params_tail = pnode;
          else {
            params_tail->next = pnode;
            params_tail = pnode;
          }

          if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',')
            adv(ctx);
        }
        fnode->as.func_def.params = params_head;
        adv(ctx);

        push_node(ctx, fnode);
        append_stmt(&root, fnode);

        push_state(ctx, STATE_GLOBAL);
        push_state(ctx, STATE_IN_FUNC);
        break;
      } else {
        if (type.is_async) {
          fprintf(stderr,
                  "Error: 'async' cannot be applied to variable '%.*s' at line "
                  "%u\n",
                  name.len, name.start, ctx->lex->line);
          return false;
        }
        if (type.is_inline) {
          fprintf(stderr,
                  "Error: 'inline' cannot be applied to variable '%.*s' at line "
                  "%u\n",
                  name.len, name.start, ctx->lex->line);
          return false;
        }
        // Construct VAR ast node and push
        AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
        vnode->as.var_decl.type = type;
        vnode->as.var_decl.id = name;
        if (ctx->curr.type == TOKEN_ASSIGN) {
          adv(ctx);
          push_node(ctx, vnode);
          append_stmt(&root->as.block.first_stmt, vnode);

          push_state(ctx, STATE_GLOBAL);
          push_state(ctx, STATE_VAR_INIT_DONE);
          push_state(ctx, STATE_IN_EXPR);
          break;
        }
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';')
          adv(ctx);
        break;
        // Var definitions could be if statement based etc.
      }
    }
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
      push_state(ctx, STATE_GLOBAL);
      break;
    }
    fprintf(stderr, "Unexpected token %.*s in global scope at line %u\n",
            ctx->curr.len, ctx->curr.start, ctx->lex->line);
    break;
  }
  case STATE_EXPR_STMT_DONE: {
    AstNode *expr_node = pop_node(ctx);
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

    if (parent->type == AST_VAR_DECL) {
      parent->as.var_decl.init = expr_node;
    } else if (parent->type == AST_BLOCK || parent->type == AST_PROGRAM) {
      append_stmt(&parent->as.block.first_stmt, expr_node);
    } else {
      fprintf(stderr,
              "Parser Error: Unexpected context for expression at line %u\n",
              ctx->lex->line);
      return false;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    }
    break;
  }
  case STATE_IN_EXPR: {
    if (ctx->curr.type == TOKEN_EOF ||
        (ctx->curr.type == TOKEN_PUNC &&
         (*ctx->curr.start == ';' || *ctx->curr.start == ')' ||
          *ctx->curr.start == ',' || *ctx->curr.start == '}' ||
          *ctx->curr.start == ']'))) {

      ctx->expect_operand = true;

      while (ctx->op_count > 0 &&
             *ctx->op_stack[ctx->op_count - 1].op.start != '(') {
        apply_op(ctx);
      }
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
      if (!ctx->expect_operand) {
        Token open_paren = ctx->curr;

        AstNode *caller = pop_node(ctx);
        AstNode *call_node = new_node(ctx->arena, AST_FUNC_CALL);
        call_node->as.func_call.caller = caller;
        push_node(ctx, call_node);
        adv(ctx);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
          adv(ctx);
          push_state(ctx, STATE_IN_EXPR);
          break;
        }
        push_op(ctx, open_paren, false, false);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_FUNC_ARGS);
        break;
      } else {
        push_op(ctx, ctx->curr, false, false);
        ctx->expect_operand = true;
        adv(ctx);
        push_state(ctx, STATE_IN_EXPR);
        break;
      }
    }

    bool is_async_block = false;
    if (ctx->curr.type == TOKEN_KW && ctx->curr.len == 5 &&
        strncmp(ctx->curr.start, "async", 5) == 0) {
      Token next = peek_token(ctx->lex);
      if (next.type == TOKEN_PUNC && *next.start == '{') {
        is_async_block = true;
        adv(ctx);
      }
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *block_node = new_node(ctx->arena, AST_BLOCK);
      block_node->as.block.is_async = is_async_block;
      push_node(ctx, block_node);

      push_state(ctx, STATE_IN_EXPR);
      push_state(ctx, STATE_BLOCK_EXPR_DONE);
      push_state(ctx, STATE_PARSE_BLOCK);

      // Ensure block internals dont consume outer operators
      Token dummy = {.start = "(", .len = 1, .type = TOKEN_PUNC};
      push_op(ctx, dummy, false, false);

      ctx->expect_operand = false;
      break;
    }

    if (ctx->curr.type == TOKEN_IDENTIF || is_lit_type(ctx->curr.type)) {
      ctx->expect_operand = false;

      ASTN_TYPE node_type;

      switch (ctx->curr.type) {
      case TOKEN_IDENTIF:
        node_type = AST_IDENTIF;
        break;
      case TOKEN_NUM_LIT:
        node_type = AST_NUM_LIT;
        break;
      case TOKEN_STR_LIT:
        node_type = AST_STR_LIT;
        break;
      case TOKEN_CHAR_LIT:
        node_type = AST_CHAR_LIT;
        break;
      case TOKEN_BOOL_LIT:
        node_type = AST_BOOL_LIT;
        break;
      default:
        return false;
      }

      AstNode *node = new_node(ctx->arena, node_type);

      if (node_type == AST_STR_LIT)
        node->as.str_lit.val = ctx->curr;
      else if (node_type == AST_CHAR_LIT)
        node->as.char_lit.val = ctx->curr;
      else if (node_type == AST_NUM_LIT)
        node->as.num_lit.val = ctx->curr;
      else
        node->as.identif.val = ctx->curr;

      push_node(ctx, node);
      adv(ctx);
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_OP || ctx->curr.type == TOKEN_COMPARE ||
        ctx->curr.type == TOKEN_ASSIGN) {
      bool is_unary = false;
      bool is_postfix = false;

      if (ctx->expect_operand) {
        is_unary = true;
      } else {
        if (ctx->curr.len == 2 && (strncmp(ctx->curr.start, "++", 2) == 0 ||
                                   strncmp(ctx->curr.start, "--", 2) == 0)) {
          is_unary = true;
          is_postfix = true;
        } else {
          ctx->expect_operand = true;
        }
      }

      int current_prec = get_precedence(ctx->curr, is_unary, is_postfix);

      bool left_assoc =
          !(is_unary && !is_postfix) && (ctx->curr.type != TOKEN_ASSIGN);

      while (ctx->op_count > 0 &&
             *ctx->op_stack[ctx->op_count - 1].op.start != '(') {
        OpInfo top_op = ctx->op_stack[ctx->op_count - 1];
        int top_prec =
            get_precedence(top_op.op, top_op.is_unary, top_op.is_postfix);

        if ((left_assoc && top_prec >= current_prec) ||
            (!left_assoc && top_prec > current_prec)) {
          apply_op(ctx);
        } else {
          break;
        }
      }

      push_op(ctx, ctx->curr, is_unary, is_postfix);
      adv(ctx);
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '[') {
      if (ctx->expect_operand) {
				// Array lit
        adv(ctx);
        AstNode *array_node = new_node(ctx->arena, AST_ARRAY_LIT);
        push_node(ctx, array_node);
        push_state(ctx, STATE_IN_ARRAY_LIT);
        break;
      } else {
				// Indexing
        adv(ctx);
        AstNode *base = pop_node(ctx);
        AstNode *idx_node = new_node(ctx->arena, AST_INDEX);
        idx_node->as.index.base = base;
        push_node(ctx, idx_node);

				// Preventing from eating outer opts
        Token dummy = {.start = "(", .len = 1, .type = TOKEN_PUNC};
        push_op(ctx, dummy, false, false);

        push_state(ctx, STATE_IN_EXPR);
        push_state(ctx, STATE_INDEX_DONE);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_EXPR);
        break;
      }
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '.') {
      if (ctx->expect_operand) {
        fprintf(stderr, "Error: Expected identifier before '.'\n");
        return false;
      }
      adv(ctx);
      if (ctx->curr.type != TOKEN_IDENTIF) {
        fprintf(stderr, "Error: Expected identifier after '.' at line %u\n",
                ctx->lex->line);
        return false;
      }

      AstNode *base = pop_node(ctx);
      AstNode *mem_node = new_node(ctx->arena, AST_MEMBER);
      mem_node->as.member.base = base;
      mem_node->as.member.name = ctx->curr;

      push_node(ctx, mem_node);
      adv(ctx);

      ctx->expect_operand = false;
      push_state(ctx,
                 STATE_IN_EXPR); // Check for more
      break;
    }

    fprintf(stderr, "Unexpected token %.*s in expression at line %u\n",
            ctx->curr.len, ctx->curr.start, ctx->lex->line);
    return false;
  }

  case STATE_INDEX_DONE: {
    AstNode *index_expr = pop_node(ctx);
    AstNode *idx_node = ctx->node_stack[ctx->node_count - 1];
    idx_node->as.index.index = index_expr;

		// Remove the dummy
    if (ctx->op_count > 0 &&
        *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
      ctx->op_count--;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      ctx->expect_operand = false;
    } else {
      fprintf(stderr, "Expected ']' after index at line %u\n", ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_IN_ARRAY_LIT: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      ctx->expect_operand = false;
      break;
    }

    push_state(ctx, STATE_IN_ARRAY_LIT);
    push_state(ctx, STATE_ARRAY_ELEMENT_DONE);
    ctx->expect_operand = true;
    push_state(ctx, STATE_IN_EXPR);
    break;
  }

  case STATE_ARRAY_ELEMENT_DONE: {
    AstNode *element_expr = pop_node(ctx);
    AstNode *array_node = ctx->node_stack[ctx->node_count - 1];

    // Append the element to the array's linked list
    if (array_node->as.array_lit.elements == NULL) {
      array_node->as.array_lit.elements = element_expr;
    } else {
      AstNode *curr = array_node->as.array_lit.elements;
      while (curr->next)
        curr = curr->next;
      curr->next = element_expr;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ']') {
      adv(ctx);
      pop_state(ctx);
      ctx->expect_operand = false;
    } else {
      fprintf(stderr, "Expected ',' or ']' in array literal at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }
  case STATE_BLOCK_EXPR_DONE: {
    // Remove dummy
    if (ctx->op_count > 0 &&
        *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
      ctx->op_count--;
    }
    break;
  }
  case STATE_IN_FUNC: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      AstNode *func_node = ctx->node_stack[ctx->node_count - 1];
      AstNode *block_node = new_node(ctx->arena, AST_BLOCK);
      func_node->as.func_def.block = block_node;

      push_node(ctx, block_node);
      push_state(ctx, STATE_PARSE_BLOCK);
      adv(ctx);
    } else {
      fprintf(stderr, "Expected '{' to start function body at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_BLOCK_DONE: {
    pop_node(ctx);
    break;
  }

  case STATE_PARSE_BLOCK: {
    AstNode *current_block = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);

      AstNode *nested_block = new_node(ctx->arena, AST_BLOCK);
      append_stmt(&current_block->as.block.first_stmt, nested_block);

      push_state(ctx, STATE_PARSE_BLOCK);
      push_state(ctx, STATE_BLOCK_DONE);
      push_node(ctx, nested_block);
      push_state(ctx, STATE_PARSE_BLOCK);
      break;
    }

    push_state(ctx, STATE_PARSE_BLOCK);

    if (ctx->curr.type == TOKEN_KW) {
      if (strncmp(ctx->curr.start, "if", 2) == 0) {
        adv(ctx);
        AstNode *if_node = new_node(ctx->arena, AST_IF);
        append_stmt(&current_block->as.block.first_stmt, if_node);
        push_node(ctx, if_node);

        push_state(ctx, STATE_IF_BODY_DONE);
        push_state(ctx, STATE_IF_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '(' after if\n");
          return false;
        }
        break;
      } else if (strncmp(ctx->curr.start, "while", 5) == 0) {
        adv(ctx);
        AstNode *while_node = new_node(ctx->arena, AST_WHILE);
        append_stmt(&current_block->as.block.first_stmt, while_node);
        push_node(ctx, while_node);

        push_state(ctx, STATE_WHILE_BODY_DONE);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, STATE_WHILE_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '(' after while\n");
          return false;
        }
        break;
      } else if (strncmp(ctx->curr.start, "defer", 5) == 0) {
        adv(ctx);
        AstNode *defer_node = new_node(ctx->arena, AST_DEFER);
        append_stmt(&current_block->as.block.first_stmt, defer_node);

        push_node(ctx, defer_node);
        push_state(ctx, STATE_DEFER_DONE);

        push_state(ctx, STATE_IN_EXPR);
        break;
      } else if (strncmp(ctx->curr.start, "ret", 3) == 0) {
        Token ret_token = ctx->curr;
        adv(ctx);

        AstNode *ret_node = new_node(ctx->arena, AST_RET);
        ret_node->as.ret_stmt.ret_kw = ret_token;

        append_stmt(&current_block->as.block.first_stmt, ret_node);

        push_node(ctx, ret_node);

        push_state(ctx, STATE_RET_DONE);
        push_state(ctx, STATE_IN_EXPR);
        break;
      } else if (strncmp(ctx->curr.start, "for", 3) == 0) {
        adv(ctx);
        AstNode *for_node = new_node(ctx->arena, AST_FOR);
        append_stmt(&current_block->as.block.first_stmt, for_node);
        push_node(ctx, for_node);

        push_state(ctx, STATE_FOR_BODY_DONE);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, STATE_FOR_INC_DONE);
        push_state(ctx, STATE_IN_EXPR);
        push_state(ctx, STATE_FOR_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        push_state(ctx, STATE_FOR_INIT_START);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '(' after for\n");
          return false;
        }
        break;
      } 
    }
    if (is_type(ctx)) {
      DataType type = parse_type(ctx);
      if (type.is_async) {
        fprintf(stderr,
                "Error: 'async' cannot be applied to variables at line %u\n",
                ctx->lex->line);
        return false;
      }
      if (type.is_inline) {
        fprintf(stderr,
                "Error: 'inline' cannot be applied to variables at line %u\n",
                ctx->lex->line);
        return false;
      }
      if (ctx->curr.type != TOKEN_IDENTIF) {
        fprintf(stderr, "Expected identifier after type at line %u\n",
                ctx->lex->line);
        return false;
      }
      Token name = ctx->curr;
      adv(ctx);

      AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
      vnode->as.var_decl.type = type;
      vnode->as.var_decl.id = name;
      append_stmt(&current_block->as.block.first_stmt, vnode);

      if (ctx->curr.type == TOKEN_ASSIGN) {
        adv(ctx);
        push_node(ctx, vnode);
        push_state(ctx, STATE_VAR_INIT_DONE);
        ctx->expect_operand = true;
        push_state(ctx, STATE_IN_EXPR);
      } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        adv(ctx);
      } else {
        fprintf(stderr, "Expected ';' or '=' after variable name at line %u\n",
                ctx->lex->line);
        return false;
      }
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
      break;
    }

    if (ctx->curr.type == TOKEN_IDENTIF || is_lit_type(ctx->curr.type) ||
        ctx->curr.type == TOKEN_OP) {
      push_state(ctx, STATE_EXPR_STMT_DONE);
      push_state(ctx, STATE_IN_EXPR);
    } else {
      fprintf(stderr, "Unexpected token %.*s in block at line %u\n",
              ctx->curr.len, ctx->curr.start, ctx->lex->line);
      return false;
    }
    break;
    break;
  }

  case STATE_FOR_INIT_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.init = init_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ';' after for-loop initialization at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_FOR_INIT_START: {
    bool is_decl = false;

    if (is_builtin_type_kw(ctx->curr) ||
        (ctx->curr.type == TOKEN_KW &&
         (strncmp(ctx->curr.start, "mut", 3) == 0 ||
          strncmp(ctx->curr.start, "static", 6) == 0))) {
      is_decl = true;
    } else if (ctx->curr.type == TOKEN_IDENTIF) {
      Token next = peek_token(ctx->lex);
      if (next.type == TOKEN_IDENTIF) {
        is_decl = true;
      }
    }

    if (is_decl) {
      DataType type = parse_type(ctx);
      if (type.is_async) {
        fprintf(stderr,
                "Error: 'async' cannot be applied to variables at line %u\n",
                ctx->lex->line);
        return false;
      }
      if (type.is_inline) {
        fprintf(stderr,
                "Error: 'inline' cannot be applied to variables at line %u\n",
                ctx->lex->line);
        return false;
      }
      if (ctx->curr.type != TOKEN_IDENTIF) {
        fprintf(stderr, "Expected identifier after type at line %u\n",
                ctx->lex->line);
        return false;
      }
      Token name = ctx->curr;
      adv(ctx);

      AstNode *vnode = new_node(ctx->arena, AST_VAR_DECL);
      vnode->as.var_decl.type = type;
      vnode->as.var_decl.id = name;

      push_node(ctx, vnode);

      if (ctx->curr.type == TOKEN_ASSIGN) {
        adv(ctx);
        push_state(ctx, STATE_FOR_INIT_DECL_DONE);
        push_state(ctx, STATE_IN_EXPR);
      } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
        AstNode *var_node = pop_node(ctx);
        AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
        for_node->as.for_loop.init = var_node;
        adv(ctx);
      } else {
        fprintf(stderr,
                "Expected '=' or ';' after variable declaration in for loop "
                "at line %u\n",
                ctx->lex->line);
        return false;
      }
    } else if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
      for_node->as.for_loop.init = NULL;
      adv(ctx);
    } else {
      push_state(ctx, STATE_FOR_INIT_DONE);
      push_state(ctx, STATE_IN_EXPR);
    }
    break;
  }

  case STATE_FOR_INIT_DECL_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *var_node = pop_node(ctx);
    var_node->as.var_decl.init = init_expr;

    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.init = var_node;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      fprintf(stderr,
              "Expected ';' after for-loop variable declaration at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_FOR_COND_DONE: {
    AstNode *cond_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.check = cond_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ';' after for-loop condition at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_FOR_INC_DONE: {
    AstNode *inc_expr = pop_node(ctx);
    AstNode *for_node = ctx->node_stack[ctx->node_count - 1];
    for_node->as.for_loop.inc = inc_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ')' after for-loop increment at line %u\n",
              ctx->lex->line);
      return false;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
      adv(ctx);
    } else {
      fprintf(stderr, "Expected '{' to start for-loop body at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }

  case STATE_FOR_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *for_node = pop_node(ctx);
    for_node->as.for_loop.action = body_block;
    break;
  }

  case STATE_WHILE_COND_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *while_node = ctx->node_stack[ctx->node_count - 1];
    while_node->as.while_loop.check = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ')' after while-condition\n");
      return false;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
    } else {
      fprintf(stderr, "Expected '{' for while body\n");
      return false;
    }
    break;
  }

  case STATE_WHILE_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *while_node = pop_node(ctx);
    while_node->as.while_loop.action = body_block;
    break;
  }

  case STATE_DEFER_DONE: {
    AstNode *action = pop_node(ctx);
    AstNode *defer_node = pop_node(ctx);
    defer_node->as.defer_stmt.contents = action;
    break;
  }
  case STATE_IF_COND_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.check = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ')' after if-condition at line %u\n",
              ctx->lex->line);
      return false;
    }
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
      adv(ctx);
      AstNode *body_block = new_node(ctx->arena, AST_BLOCK);
      push_node(ctx, body_block);
      push_state(ctx, STATE_PARSE_BLOCK);
    } else {
      push_state(ctx, STATE_IN_EXPR);
    }
    break;
  }
  case STATE_IF_BODY_DONE: {
    AstNode *body_block = pop_node(ctx);
    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.action = body_block;

    if (ctx->curr.type == TOKEN_KW &&
        strncmp(ctx->curr.start, "else", 4) == 0) {
      adv(ctx);

      if (ctx->curr.type == TOKEN_KW &&
          strncmp(ctx->curr.start, "if", 2) == 0) {
        // Else if block
        adv(ctx);
        AstNode *elif_node = new_node(ctx->arena, AST_IF);
        if_node->as.if_check.elseAct = elif_node;

        // just like a normal if statement
        push_node(ctx, elif_node);
        push_state(ctx, STATE_IF_BODY_DONE);
        push_state(ctx, STATE_PARSE_BLOCK);
        push_state(ctx, STATE_IF_COND_DONE);
        push_state(ctx, STATE_IN_EXPR);

        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '(') {
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '(' after else if\n");
          return false;
        }
      } else {
        // Else block
        if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '{') {
          AstNode *else_block = new_node(ctx->arena, AST_BLOCK);
          if_node->as.if_check.elseAct = else_block;

          push_state(ctx, STATE_ELSE_BODY_DONE);
          push_node(ctx, else_block);
          push_state(ctx, STATE_PARSE_BLOCK);
          adv(ctx);
        } else {
          fprintf(stderr, "Expected '{' after else\n");
          return false;
        }
      }
    } else {
      // No else
      pop_node(ctx);
    }
    break;
  }

  case STATE_ELSE_BODY_DONE: {
    pop_node(ctx);
    pop_node(ctx);
    break;
  }
  case STATE_IN_IF_EXPECT_COND: {
    AstNode *cond_expr = pop_node(ctx);

    AstNode *if_node = ctx->node_stack[ctx->node_count - 1];
    if_node->as.if_check.check = cond_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ')' after if-condition at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }
  case STATE_IN_STRUCT_DEF:
  case STATE_IN_UNION_DEF: {
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

		skip_irrelevant(ctx->lex);

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      pop_node(ctx);
      break;
    }

    if (!is_type(ctx)) {
      fprintf(stderr, "Expected type in struct/union definition at line %u\n",
              ctx->lex->line);
      return false;
    }

    DataType field_type = parse_type(ctx);

    if (field_type.is_async) {
      fprintf(stderr,
              "Error: 'async' cannot be applied to struct/union fields at line "
              "%u\n",
              ctx->lex->line);
      return false;
    }
    if (field_type.is_inline) {
      fprintf(stderr,
              "Error: 'inline' cannot be applied to struct/union fields at line "
              "%u\n",
              ctx->lex->line);
      return false;
    }

    if (ctx->curr.type != TOKEN_IDENTIF) {
      fprintf(stderr, "Expected field identifier at line %u\n", ctx->lex->line);
      return false;
    }

    Token field_name = ctx->curr;
    adv(ctx);

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ';' after field declaration at line %u\n",
              ctx->lex->line);
      return false;
    }

    AstNode *field_node = new_node(ctx->arena, AST_VAR_DECL);
    field_node->as.var_decl.type = field_type;
    field_node->as.var_decl.id = field_name;

    AstNode **target_list = (parent->type == AST_STRUCT)
                                ? &parent->as.struct_def.contents
                                : &parent->as.union_def.contents;

    append_stmt(target_list, field_node);

    push_state(ctx, current_state);
    break;
  }
  case STATE_IN_ENUM_DEF: {
    AstNode *parent = ctx->node_stack[ctx->node_count - 1];

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == '}') {
      adv(ctx);
      pop_node(ctx);
      break;
    }

    if (ctx->curr.type != TOKEN_IDENTIF) {
      fprintf(stderr, "Expected identifier in enum at line %u\n",
              ctx->lex->line);
      return false;
    }

    AstNode *enum_member = new_node(ctx->arena, AST_ENUM_MEMBER);
    enum_member->as.enum_member.name = ctx->curr;
    adv(ctx);

    if (parent->as.enum_def.contents == NULL) {
      parent->as.enum_def.contents = enum_member;
    } else {
      AstNode *tail = parent->as.enum_def.contents;
      while (tail->next != NULL)
        tail = tail->next;
      tail->next = enum_member;
    }

    if (ctx->curr.type == TOKEN_ASSIGN) {
      adv(ctx);
      push_state(ctx, STATE_IN_ENUM_DEF);
      push_state(ctx,
                 STATE_ENUM_MEMBER_DONE);

      push_node(ctx, enum_member);

      ctx->expect_operand = true;
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    }

    push_state(ctx, current_state);
    break;
  }

  case STATE_ENUM_MEMBER_DONE: {
    AstNode *expr = pop_node(ctx);
    AstNode *enum_member = pop_node(ctx);

    enum_member->as.enum_member.val = expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    }
    break;
  }
  case STATE_RET_DONE: {
    AstNode *expr_result = pop_node(ctx);
    AstNode *ret_node = pop_node(ctx);

    ret_node->as.ret_stmt.expr = expr_result;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else if (expr_result->type != AST_BLOCK) {
      fprintf(stderr,
              "Error: Expected ';' after return expression at line %u\n",
              ctx->lex->line);
      return false;
    }
    break;
  }
  case STATE_VAR_INIT_DONE: {
    AstNode *init_expr = pop_node(ctx);
    AstNode *var_node = ctx->node_stack[ctx->node_count - 1];

    var_node->as.var_decl.init = init_expr;

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ';') {
      adv(ctx);
    } else {
      fprintf(stderr, "Expected ';' after variable declaration at line %u\n",
              ctx->lex->line);
      return false;
    }

    pop_node(ctx);
    break;
  }
  case STATE_IN_FUNC_ARGS: {
    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ')') {
      adv(ctx);
      if (ctx->op_count > 0 &&
          *ctx->op_stack[ctx->op_count - 1].op.start == '(') {
        ctx->op_count--;
      }
      ctx->expect_operand = false;
      push_state(ctx, STATE_IN_EXPR);
      break;
    }

    if (ctx->curr.type == TOKEN_PUNC && *ctx->curr.start == ',') {
      adv(ctx);
    }

    push_state(ctx, STATE_IN_FUNC_ARGS);
    push_state(ctx, STATE_ARG_DONE);
    push_state(ctx, STATE_IN_EXPR);
    break;
  }

  case STATE_ARG_DONE: {
    AstNode *arg_expr = pop_node(ctx);
    AstNode *call_node = ctx->node_stack[ctx->node_count - 1];

    if (call_node->as.func_call.args == NULL) {
      call_node->as.func_call.args = arg_expr;
    } else {
      AstNode *tail = call_node->as.func_call.args;
      while (tail->next)
        tail = tail->next;
      tail->next = arg_expr;
    }
    break;
  }
  }
  return true;
}

bool parse(ParseCtx *ctx) {
  push_state(ctx, STATE_GLOBAL);
  while (ctx->state_count > 0 && ctx->curr.type != TOKEN_EOF) {
    if (!parse_step(ctx))
      return false;
  }
  return true;
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
  pctx.state_stack = malloc(sizeof(ParseState) * pctx.state_cap);

  AstNode *root = new_node(pctx.arena, AST_PROGRAM);
  push_node(&pctx, root);

  if (!parse(&pctx)) {
    fprintf(stderr, "Some error occurred during parsing");
    arena_free_all(&arena);
    free((void *)file);
    exit(127);
  }

  print_ast(root);

  free(pctx.state_stack);
  free(pctx.node_stack);
  free(pctx.op_stack);
  arena_free_all(&arena);
  free((void *)file);
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
