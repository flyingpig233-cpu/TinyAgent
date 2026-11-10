#ifndef TINY_AGENT_PLATFORM_H
#define TINY_AGENT_PLATFORM_H

/* Small POSIX/Windows portability layer used by the library and the CLI.
 * This header is internal: it is not installed and is not part of the public
 * API. All paths and strings are UTF-8. */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <wchar.h>
#endif

/* Monotonic milliseconds. Returns -1 when the clock is unavailable. */
long long ta_monotonic_ms(void);

/* 1 when fd is an interactive terminal (0 = stdin, 1 = stdout, 2 = stderr). */
int ta_isatty(int fd);

/* malloc-owned copy of text; NULL on allocation failure or NULL input. */
char *ta_strdup(const char *text);

/* ASCII case-insensitive comparison; returns <0, 0 or >0. */
int ta_strcasecmp(const char *a, const char *b);

/* Normalized file metadata. Times are expressed as Unix epoch nanoseconds
 * when the platform provides that precision; otherwise the field is 0. */
typedef struct {
  int regular;
  int directory;
  int symlink;
  unsigned int mode;
  uint64_t size;
  int64_t mtime;
  uint64_t mtime_ns;
  uint64_t ctime_ns;
  uint64_t device;
  uint64_t inode;
} TaFileStat;

/* Stat without following a final symlink. Returns 0 on success, -1 on error
 * with errno set. */
int ta_fs_lstat(const char *path, TaFileStat *out);

/* Stat an open descriptor. Returns 0 on success, -1 on error. */
int ta_fs_fstat(int fd, TaFileStat *out);

/* 1 when both descriptors still refer to the same unchanged file. */
int ta_fs_same_file(const TaFileStat *a, const TaFileStat *b);

/* Open an existing file for reading. Returns a descriptor or -1. */
int ta_fs_open_read(const char *path);
int ta_fs_close(int fd);
ptrdiff_t ta_fs_read(int fd, void *buffer, size_t count);
ptrdiff_t ta_fs_write(int fd, const void *buffer, size_t count);
int ta_fs_sync(int fd);
int ta_fs_chmod(int fd, unsigned int mode);

/* Create a unique file from a template ending in "XXXXXX" and return an open
 * descriptor. The template is updated in place. */
int ta_fs_mkstemp(char *path_template);

/* Rename, replacing the destination. Returns 0 on success. */
int ta_fs_rename(const char *from, const char *to);

/* Publish "from" as "to" only when "to" does not exist. Returns 0 on success.
 */
int ta_fs_publish_noreplace(const char *from, const char *to);

int ta_fs_unlink(const char *path);

/* Directory iteration. ta_dir_next() returns 1 for an entry (name is
 * malloc-owned and must be freed by the caller), 0 at end, -1 on error.
 * Dot entries are skipped. */
typedef struct TaDir TaDir;
TaDir *ta_dir_open(const char *path);
int ta_dir_next(TaDir *dir, char **name, TaFileStat *info);
void ta_dir_close(TaDir *dir);

#if defined(_WIN32)
/* UTF-8 to UTF-16 and back. Both return malloc-owned strings. */
wchar_t *ta_utf8_to_wide(const char *text);
char *ta_wide_to_utf8(const wchar_t *text);
#endif

#endif /* TINY_AGENT_PLATFORM_H */
