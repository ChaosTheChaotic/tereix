#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

typedef enum {
  CMD_BUILD,
  CMD_FMT,
} Cmd;

typedef struct {
  Cmd cmd;
  bool help;
  bool print_ast;
  const char *input_file;
  union {
    struct {
      const char *compiler;
      const char *output_file;
      const char **extra_cflags;
      unsigned int extra_cflag_count;
      bool keep_c_files;
    } build;
    struct {
      bool write;
      bool check;
      bool recursive;
    } fmt;
  } as;
} CompileOptions;

int parse_options(int argc, char **argv, CompileOptions *opts);

void print_usage(const char *progname);

#endif
