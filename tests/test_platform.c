#include "infra/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  char path[] = "tiny-agent-platform-XXXXXX";
  int fd = ta_fs_mkstemp(path);
  CHECK(fd >= 0);

  const char text[] = "hello platform\n";
  CHECK(ta_fs_write(fd, text, sizeof(text) - 1) ==
        (ptrdiff_t)(sizeof(text) - 1));
  CHECK(ta_fs_sync(fd) == 0);
  CHECK(ta_fs_close(fd) == 0);

  TaFileStat before;
  CHECK(ta_fs_lstat(path, &before) == 0);
  CHECK(before.regular && !before.directory && !before.symlink);
  CHECK(before.size == (uint64_t)(sizeof(text) - 1));

  fd = ta_fs_open_read(path);
  CHECK(fd >= 0);
  char buffer[64] = {0};
  ptrdiff_t count = ta_fs_read(fd, buffer, sizeof(buffer) - 1);
  CHECK(count == (ptrdiff_t)(sizeof(text) - 1));
  CHECK(memcmp(buffer, text, sizeof(text) - 1) == 0);
  CHECK(ta_fs_close(fd) == 0);

  TaFileStat after;
  CHECK(ta_fs_lstat(path, &after) == 0);
  CHECK(ta_fs_same_file(&before, &after));

  char renamed[] = "tiny-agent-platform-renamed";
  CHECK(ta_fs_rename(path, renamed) == 0);
  CHECK(ta_fs_lstat(renamed, &after) == 0);
  CHECK(ta_fs_unlink(renamed) == 0);
  CHECK(ta_fs_lstat(renamed, &after) != 0);

  TaDir *dir = ta_dir_open(".");
  CHECK(dir != NULL);
  char *name = NULL;
  TaFileStat info;
  int entries = 0;
  int next;
  while ((next = ta_dir_next(dir, &name, &info)) == 1) {
    CHECK(name != NULL && name[0] != '\0');
    free(name);
    name = NULL;
    ++entries;
  }
  CHECK(next == 0);
  CHECK(entries > 0);
  ta_dir_close(dir);

  char *copy = ta_strdup("AbC");
  CHECK(copy != NULL && strcmp(copy, "AbC") == 0);
  CHECK(ta_strcasecmp(copy, "abc") == 0);
  free(copy);

  CHECK(ta_monotonic_ms() >= 0);
  (void)ta_isatty(0);
  (void)ta_isatty(1);
  (void)ta_isatty(2);
  return 0;
}
