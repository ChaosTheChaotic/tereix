#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

typedef struct {
  const char *compiler;
  const char *output_file;
  const char **extra_cflags;
  unsigned int extra_cflag_count;
  const char *input_file;
  bool print_ast;
  bool help;
} CompileOptions;

int parse_options(int argc, char **argv, CompileOptions *opts);

void print_usage(const char *progname);

#endif
