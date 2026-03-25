#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>

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
  TOKEN_TYPE base_type;
  unsigned int array_dimens; // 0 not an array 1 for [] etc
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
  AST_DEFER,
  AST_FOR,
  AST_WHILE,
  AST_FUNC,
  AST_PARAM,
} ASTN_TYPE;

typedef struct {
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

void try_compile(const char *path) {
  const char *file = load_file(path);
  if (!file) {
    fprintf(stderr, "Failed to load file: %s", path);
    exit(1);
  }
  LexCtx *ctx = malloc(sizeof(LexCtx));
  if (!ctx) {
    fprintf(stderr, "Failed to malloc a lex ctx");
    free((char *)file);
    exit(1);
  }
  ctx->start = (char *)file;
  ctx->curr = (char *)file;
  ctx->line = 1;
  ctx->col = 1;
  Token t;
  while ((t = next_token(ctx)).type != TOKEN_EOF) {
    switch (t.type) {
      case TOKEN_UNKNOWN: {
	fprintf(stderr, "Encountered unknown token %.*s at line %u, col %u", t.len, t.start, ctx->line, ctx->col);
	goto err_cleanup;
      }
      case TOKEN_OP: {

	break;
      }
      case TOKEN_ASSIGN: {

	break;
      }
      case TOKEN_PUNC: {

	break;
      }
      case TOKEN_IDENTIF: {

	break;
      }
      case TOKEN_LIT: {

	break;
      }
      case TOKEN_KW: {

	break;
      }
      case TOKEN_COMPARE: {

	break;
      }
      case TOKEN_EOF: {
	fprintf(stderr, "Encounted unexpected EOF"); // Should never happen
	goto err_cleanup;
      }
    }
  }
  free((void*)file);
  free(ctx);
  return;
err_cleanup:
  free((void*)file);
  free(ctx);
  exit(127);
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
