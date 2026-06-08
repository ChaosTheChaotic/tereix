#include "cli.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *default_compiler(void) {
  const char *cc = getenv("CC");
  return cc ? cc : "cc";
}

int parse_options(int argc, char **argv, CompileOptions *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->compiler = default_compiler();

  static struct option long_options[] = {
      {"print-ast", no_argument, 0, 'p'},
      {"help", no_argument, 0, 'h'},
      {"compiler", required_argument, 0, 'c'},
      {"output", required_argument, 0, 'o'},
      {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "phc:o:", long_options,
                            &option_index)) != -1) {
    switch (opt) {
    case 'p':
      opts->print_ast = true;
      break;
    case 'h':
      opts->help = true;
      return 0;
    case 'c':
      opts->compiler = optarg;
      break;
    case 'o':
      opts->output_file = optarg;
      break;
    default:
      return -1;
    }
  }

  // Collect extra flags after optional --
  int remaining = argc - optind;
  if (remaining > 0 && strcmp(argv[optind], "--") == 0) {
    optind++;
    remaining = argc - optind;
    if (remaining > 0) {
      opts->extra_cflags = malloc((remaining + 1) * sizeof(char *));
      opts->extra_cflag_count = remaining;
      for (int i = 0; i < remaining; i++)
        opts->extra_cflags[i] = argv[optind + i];
      opts->extra_cflags[remaining] = NULL;
    }
  }
  if (optind < argc && argv[optind][0] != '-') {
    opts->input_file = argv[optind];
    optind++;
  } else {
    fprintf(stderr, "Error: no input file specified.\n");
    return -1;
  }

  // Warn about any extra positional arguments
  if (optind < argc) {
    fprintf(stderr, "Warning: extra arguments ignored: ");
    while (optind < argc)
      fprintf(stderr, "%s ", argv[optind++]);
    fprintf(stderr, "\n");
  }

  return 0;
}

void print_usage(const char *progname) {
  printf("Usage: %s [options] <input.tx>\n", progname);
  printf("Options:\n");
	printf("  --lsp			   Run as an lsp\n");
  printf("  -p, --print-ast          Print the AST after parsing\n");
  printf(
      "  -c, --compiler <cc>      Specify C compiler (default: $CC or 'cc')\n");
  printf("  -o, --output <file>      Output binary name (default: input base "
         "name)\n");
  printf("  -h, --help               Show this help message\n");
  printf("  --                       Pass following arguments directly to the "
         "C compiler\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s -p main.tx\n", progname);
  printf("  %s --compiler clang -O2 main.tx\n", progname);
  printf("  %s -c gcc-12 -- -march=native main.tx\n", progname);
}
