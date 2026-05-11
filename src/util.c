#include "util.h"
#include "arena.h"
#include <limits.h>
#include <string.h>

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
  }
  else if (*path_start == '/') {
  } else {
    return NULL;
  }

  char *resolved = realpath(path_start, NULL);
  return resolved;
}

char *uri_from_absolute(const char *absolute) {
    if (!absolute || absolute[0] != '/') return NULL;

    const char *prefix = "file://";
    char *uri = malloc(strlen(prefix) + strlen(absolute) + 1);
    if (uri) {
        sprintf(uri, "%s%s", prefix, absolute);
    }
    return uri;
}
