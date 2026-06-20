#include "cli.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_format_options(int argc, char **argv, CompileOptions *opts) {
    // Default format options
    opts->cmd = CMD_FMT;
    opts->as.fmt.write = false;
    opts->as.fmt.check = false;
    opts->as.fmt.recursive = false;

    int opt;
    while ((opt = getopt(argc, argv, "hpwcrj:")) != -1) {
        switch (opt) {
        case 'p':
          opts->print_ast = true;
          break;
        case 'h':
          opts->help = true;
          return 0;
        case 'w':
          opts->as.fmt.write = true;
          break;
        case 'c':
            opts->as.fmt.check = true;
            break;
        case 'r':
            opts->as.fmt.recursive = true;
            break;
        case 'j':
          opts->thread_count = atoi(optarg);
          if (opts->thread_count < 1) {
            fprintf(stderr, "Thread count must be at least 1.\n");
            return -1;
          }
          break;
        default:
            return -1;
        }
    }

    if (optind < argc) {
        opts->input_file = argv[optind++];
    } else {
        fprintf(stderr, "Error: no input file specified for fmt.\n");
        return -1;
    }

    if (optind < argc) {
        fprintf(stderr, "Warning: extra arguments ignored for fmt: ");
        while (optind < argc) fprintf(stderr, "%s ", argv[optind++]);
        fprintf(stderr, "\n");
    }

    return 0;
}

const char *default_compiler(void) {
  const char *cc = getenv("CC");
  return cc ? cc : "cc";
}

int parse_options(int argc, char **argv, CompileOptions *opts) {
  memset(opts, 0, sizeof(*opts));

  if (argc >= 2 && strcmp(argv[1], "fmt") == 0) {
    // argv[1] as command, rest as fmt options
    return parse_format_options(argc - 1, argv + 1, opts);
  }

	opts->cmd = CMD_BUILD;
  opts->as.build.compiler = default_compiler();

  static struct option long_options[] = {
      {"print-ast", no_argument, 0, 'p'},
      {"help", no_argument, 0, 'h'},
      {"compiler", required_argument, 0, 'c'},
      {"output", required_argument, 0, 'o'},
      {"keep-c", no_argument, 0, 'k'},
      {"threads", required_argument, 0, 'j'},
      {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "phc:o:kj:", long_options,
                            &option_index)) != -1) {
    switch (opt) {
    case 'p':
      opts->print_ast = true;
      break;
    case 'h':
      opts->help = true;
      return 0;
    case 'c':
      opts->as.build.compiler = optarg;
      break;
    case 'o':
      opts->as.build.output_file = optarg;
      break;
    case 'k':
      opts->as.build.keep_c_files = true;
      break;
    case 'j':
      opts->thread_count = atoi(optarg);
      if (opts->thread_count < 1) {
        fprintf(stderr, "Thread count must be at least 1.\n");
        return -1;
      }
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
      opts->as.build.extra_cflags = malloc((remaining + 1) * sizeof(char *));
      opts->as.build.extra_cflag_count = remaining;
      for (int i = 0; i < remaining; i++)
        opts->as.build.extra_cflags[i] = argv[optind + i];
      opts->as.build.extra_cflags[remaining] = NULL;
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
  printf("Usage: %s [--lsp] [command] [options] [input...]\n", progname);
  printf("\nCommands:\n");
  printf("  [options] <input.tx>	       Compile a Tereix program (default)\n");
  printf("  fmt [options] <input.tx>     Format source code\n");
  printf("\nCommon options:\n");
  printf("  -h, --help                   Show this help message\n");
  printf("  -p, --print-ast              Print the AST after parsing\n");
  printf("  -j, --threads <N>          Use N threads for parsing (default: "
         "auto)\n");
  printf("\nBuild options:\n");
  printf("  -c, --compiler <cc>          Specify C compiler (default: $CC or "
         "'cc')\n");
  printf("  -o, --output <file>          Output binary name (default: input "
         "base name)\n");
  printf(
      "  -k, --keep-c                 Keep generated .c files in .tx_cache/\n");
  printf("  --                           Pass following arguments directly to "
         "the C compiler\n");
  printf("\nFmt options:\n");
  printf(
      "  -w, --write                  Write formatted output back to file\n");
  printf("  -c, --check                  Check if files are formatted (exit "
         "with status)\n");
  printf("  -r, --recursive              Process directories recursively\n");
  printf("\nExamples:\n");
  printf("  %s -p main.tx\n", progname);
  printf("  %s --compiler clang -O2 main.tx\n", progname);
  printf("  %s fmt -w main.tx\n", progname);
  printf("  %s fmt -r src/\n", progname);
}
