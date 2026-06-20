#include "util.h"
#include "arena.h"
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

const char *resolve_alloc(Arena *arena, const char *rel_path) {
  char temp[PATH_MAX];
  if (realpath(rel_path, temp) == NULL)
    return NULL;
  size_t len = strlen(temp) + 1;
  char *perm_path = arena_alloc(arena, len);
  memcpy(perm_path, temp, len);
  return perm_path;
}

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

char *absolute_from_uri(const char *uri) {
  const char *scheme = "file://";
  if (strncmp(uri, scheme, 7) != 0)
    return NULL;

  const char *path_start = uri + 7;

  if (strncmp(path_start, "localhost/", 10) == 0) {
    path_start += 9; // Point to the '/'
  } else if (*path_start == '/') {
  } else {
    return NULL;
  }

  char *resolved = realpath(path_start, NULL);
  return resolved;
}

char *uri_from_absolute(const char *absolute) {
  if (!absolute || absolute[0] != '/')
    return NULL;

  const char *prefix = "file://";
  char *uri = malloc(strlen(prefix) + strlen(absolute) + 1);
  if (uri) {
    sprintf(uri, "%s%s", prefix, absolute);
  }
  return uri;
}

const char *extract_mod_name(Arena *arena, const char *abs_path) {
  const char *base = strrchr(abs_path, '/');
  base = base ? base + 1 : abs_path;

  const char *ext = strrchr(base, '.');
  size_t len = ext ? (size_t)(ext - base) : strlen(base);

  char *mod_name = arena_alloc(arena, len + 1);
  strncpy(mod_name, base, len);
  mod_name[len] = '\0';

  return mod_name;
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

bool file_is_identical(const char *path, StringBuilder *code) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size != (long)code->len) {
    fclose(f);
    return false;
  }

  char *buf = malloc(size);
  bool same = false;
  if (fread(buf, 1, size, f) == (size_t)size) {
    same = (memcmp(buf, code->buf, size) == 0);
  }
  free(buf);
  fclose(f);
  return same;
}

void ensure_cache_dir() {
  struct stat st = {0};
  if (stat(".tx_cache", &st) == -1) {
#if defined(_WIN32)
    mkdir(".tx_cache");
#else
    mkdir(".tx_cache", 0700);
#endif
  }
}

const char *load_file_into_arena(Arena *arena, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long fsize = ftell(fp);
  if (fsize < 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  char *buf = arena_alloc(arena, fsize + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t read = fread(buf, 1, fsize, fp);
  fclose(fp);
  if (read != (size_t)fsize)
    return NULL;
  buf[fsize] = '\0';
  return buf;
}
