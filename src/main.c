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
  TOKEN_VARDEF,
  TOKEN_MATHOP,
  TOKEN_BITWISE,
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
  return ( c == '\n' || c == '\r' || c == '\0');
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
    
    char *rptr = file;
    char *wrptr = file;
    while (*rptr) {
      char *lstart = rptr;
      char *lend = strchr(rptr, '\n');
      size_t llen = lend ? (size_t)(lend - rptr + 1) : strlen(rptr);

      char *check = rptr;
      while (check < (lstart + llen) && isspace((unsigned char)*check) && is_newline(*check)) check++;
      bool is_blank = is_newline(*check);
      if ( ((lstart + llen) - check) < 2 || strncmp(check, "//", 2) != 0 || is_blank) {
	if (wrptr != rptr) memcpy(wrptr, rptr, llen);
	wrptr += llen;
      }
      if (!lend) break;
      rptr = lend + 1;
    }
    *wrptr = '\0';
    char *shrunk = realloc(file, (wrptr - file) + 1);
    return shrunk ? shrunk : file;
  } else {
    return NULL;
  }
}

const char *vdlist[] = {
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
};

const char *mathoplist[] = {
  "+",
  "-",
  "/",
  "*",
};

const char *bitoplist[] = {
  "^",
  "&",
  "|",
  "!",
  "<<",
  ">>",
};

inline bool is_vardef(char *start, unsigned int len) {
  for (unsigned int i = 0; i < (sizeof(vdlist) / (sizeof(vdlist[0]))); i++) {
    if (strncmp(start, vdlist[i], len) == 0) {
      return true;
    }
  }
  return false;
}

inline bool is_mathop(char op) {
  for (unsigned int i = 0; i < (sizeof(mathoplist) / sizeof(mathoplist[0])); i++) {
    if (op == *mathoplist[i]) {
      return true;
    }
  }
  return false;
}

inline bool is_bitopt(char *start, unsigned int len) {
  for (unsigned int i = 0; i < (sizeof(bitoplist) / sizeof(bitoplist[0])); i++) {
    if (strncmp(start, bitoplist[i], len) == 0) {
      return true;
    }
  }
  return false;
}

Token next_token(LexCtx *ctx) {
  while (isspace(*ctx->curr) && !is_newline(*ctx->curr)) {
    ctx->curr++;
    ctx->col++;
  }
  if (is_newline(*ctx->curr)) ctx->line++;
  ctx->start = ctx->curr;
  while (isalnum(*ctx->curr)) ctx->curr++;
  char *token_end = ctx->curr - 1;
  unsigned int len = token_end - ctx->start;
  TOKEN_TYPE type;
  if (*ctx->curr == '\0') {
    type = TOKEN_EOF;
  } else if (is_vardef(ctx->start, len)) {
    type = TOKEN_VARDEF;
  } else if (is_mathop(*token_end)) {
    type = TOKEN_MATHOP;
  } else if (is_bitopt(ctx->start, len)) {
    type = TOKEN_BITWISE;
  } else {
    type = TOKEN_UNKNOWN;
  }
  Token t = {
    .start = ctx->start,
    .len = len,
    .type = type,
  };
  return t;
}

void try_compile(const char *path) {
  const char *file = load_file(path);
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
    // Figure out what to do with tokens
  }
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    print_help();
    exit(1);
  }
  for (unsigned i = 0; i < (unsigned int)argc; i++) {
    if (check_exists(argv[i])) {
      printf("A valid file was found, attempting to compile it");
      try_compile(argv[i]);
    }
  }
  printf("No valid file found");
  print_help();
  return 1;
}
