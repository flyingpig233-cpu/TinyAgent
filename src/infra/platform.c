#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

long long ta_monotonic_ms(void) {
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
    return -1;
  if (!QueryPerformanceCounter(&counter))
    return -1;
  return (long long)((counter.QuadPart * 1000) / frequency.QuadPart);
}

int ta_isatty(int fd) {
  HANDLE handle;
  DWORD mode;
  switch (fd) {
  case 0:
    handle = GetStdHandle(STD_INPUT_HANDLE);
    break;
  case 1:
    handle = GetStdHandle(STD_OUTPUT_HANDLE);
    break;
  case 2:
    handle = GetStdHandle(STD_ERROR_HANDLE);
    break;
  default:
    return 0;
  }
  return handle != NULL && handle != INVALID_HANDLE_VALUE &&
         GetConsoleMode(handle, &mode) != 0;
}

char *ta_strdup(const char *text) {
  if (text == NULL)
    return NULL;
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (copy != NULL)
    memcpy(copy, text, length + 1);
  return copy;
}

int ta_strcasecmp(const char *a, const char *b) {
  const unsigned char *left = (const unsigned char *)a;
  const unsigned char *right = (const unsigned char *)b;
  for (;;) {
    unsigned char l = *left;
    unsigned char r = *right;
    if (l >= 'A' && l <= 'Z')
      l = (unsigned char)(l - 'A' + 'a');
    if (r >= 'A' && r <= 'Z')
      r = (unsigned char)(r - 'A' + 'a');
    if (l != r)
      return (int)l - (int)r;
    if (l == 0)
      return 0;
    ++left;
    ++right;
  }
}

wchar_t *ta_utf8_to_wide(const char *text) {
  if (text == NULL)
    return NULL;
  int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (length <= 0)
    return NULL;
  wchar_t *wide = malloc((size_t)length * sizeof(*wide));
  if (wide == NULL)
    return NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                          length) <= 0) {
    free(wide);
    return NULL;
  }
  return wide;
}

char *ta_wide_to_utf8(const wchar_t *text) {
  if (text == NULL)
    return NULL;
  int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  if (length <= 0)
    return NULL;
  char *utf8 = malloc((size_t)length);
  if (utf8 == NULL)
    return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, length, NULL, NULL) <=
      0) {
    free(utf8);
    return NULL;
  }
  return utf8;
}

static void win32_errno(DWORD error) {
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_NAME:
    errno = ENOENT;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
    errno = EACCES;
    break;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    errno = EEXIST;
    break;
  case ERROR_DIRECTORY:
    errno = ENOTDIR;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    errno = ENOMEM;
    break;
  default:
    errno = EIO;
    break;
  }
}

static wchar_t *path_to_wide(const char *path) {
  wchar_t *wide = ta_utf8_to_wide(path);
  if (wide == NULL)
    return NULL;
  size_t length = wcslen(wide);
  while (length > 1u &&
         (wide[length - 1] == L'/' || wide[length - 1] == L'\\')) {
    if (length == 3u && wide[1] == L':')
      break;
    wide[--length] = L'\0';
  }
  return wide;
}

static uint64_t filetime_ns(FILETIME time) {
  uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
  const uint64_t epoch = 116444736000000000ULL;
  if (ticks <= epoch)
    return 0;
  return (ticks - epoch) * 100ULL;
}

static void fill_file_stat(const BY_HANDLE_FILE_INFORMATION *info,
                           TaFileStat *out) {
  memset(out, 0, sizeof(*out));
  out->symlink = (info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  out->directory = (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out->regular = !out->directory && !out->symlink;
  out->mode = out->directory ? 0755u : 0644u;
  out->size = ((uint64_t)info->nFileSizeHigh << 32) | info->nFileSizeLow;
  out->mtime_ns = filetime_ns(info->ftLastWriteTime);
  out->ctime_ns = filetime_ns(info->ftCreationTime);
  out->mtime = (int64_t)(out->mtime_ns / 1000000000ULL);
  out->device = info->dwVolumeSerialNumber;
  out->inode = ((uint64_t)info->nFileIndexHigh << 32) | info->nFileIndexLow;
}

static int win_stat(const char *path, int follow, TaFileStat *out) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) {
    errno = EINVAL;
    return -1;
  }
  DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
  if (!follow)
    flags |= FILE_FLAG_OPEN_REPARSE_POINT;
  HANDLE handle =
      CreateFileW(wide, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, flags, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    win32_errno(GetLastError());
    return -1;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) {
    DWORD error = GetLastError();
    CloseHandle(handle);
    win32_errno(error);
    return -1;
  }
  CloseHandle(handle);
  fill_file_stat(&info, out);
  return 0;
}

int ta_fs_lstat(const char *path, TaFileStat *out) {
  return win_stat(path, 0, out);
}

int ta_fs_fstat(int fd, TaFileStat *out) {
  intptr_t raw = _get_osfhandle(fd);
  if (raw == -1) {
    errno = EBADF;
    return -1;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle((HANDLE)raw, &info)) {
    win32_errno(GetLastError());
    return -1;
  }
  fill_file_stat(&info, out);
  return 0;
}

int ta_fs_same_file(const TaFileStat *a, const TaFileStat *b) {
  return a->device == b->device && a->inode == b->inode && a->size == b->size &&
         a->mode == b->mode && a->mtime_ns == b->mtime_ns &&
         a->ctime_ns == b->ctime_ns;
}

int ta_fs_open_read(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) {
    errno = EINVAL;
    return -1;
  }
  int fd = _wopen(wide, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  free(wide);
  return fd;
}

int ta_fs_close(int fd) { return _close(fd); }

ptrdiff_t ta_fs_read(int fd, void *buffer, size_t count) {
  unsigned int amount = count > (size_t)INT_MAX ? (unsigned int)INT_MAX : (unsigned int)count;
  return (ptrdiff_t)_read(fd, buffer, amount);
}

ptrdiff_t ta_fs_write(int fd, const void *buffer, size_t count) {
  unsigned int amount = count > (size_t)INT_MAX ? (unsigned int)INT_MAX : (unsigned int)count;
  return (ptrdiff_t)_write(fd, buffer, amount);
}

int ta_fs_sync(int fd) { return _commit(fd); }

int ta_fs_chmod(int fd, unsigned int mode) {
  (void)fd;
  (void)mode;
  return 0;
}

int ta_fs_mkstemp(char *path_template) {
  size_t length = strlen(path_template);
  if (length < 6u || strcmp(path_template + length - 6, "XXXXXX") != 0) {
    errno = EINVAL;
    return -1;
  }
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  uint64_t seed =
      (uint64_t)GetCurrentProcessId() ^ ((uint64_t)ta_monotonic_ms() << 32);
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (int i = 0; i < 6; ++i) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      path_template[length - 6 + (size_t)i] =
          alphabet[(size_t)((seed >> 33) % (sizeof(alphabet) - 1))];
    }
    wchar_t *wide = path_to_wide(path_template);
    if (wide == NULL) {
      errno = EINVAL;
      return -1;
    }
    int fd =
        _wopen(wide, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
               _S_IREAD | _S_IWRITE);
    free(wide);
    if (fd >= 0)
      return fd;
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

int ta_fs_rename(const char *from, const char *to) {
  wchar_t *wide_from = path_to_wide(from);
  wchar_t *wide_to = path_to_wide(to);
  if (wide_from == NULL || wide_to == NULL) {
    free(wide_from);
    free(wide_to);
    errno = EINVAL;
    return -1;
  }
  BOOL result = MoveFileExW(wide_from, wide_to,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  free(wide_from);
  free(wide_to);
  if (!result) {
    win32_errno(GetLastError());
    return -1;
  }
  return 0;
}

int ta_fs_publish_noreplace(const char *from, const char *to) {
  wchar_t *wide_from = path_to_wide(from);
  wchar_t *wide_to = path_to_wide(to);
  if (wide_from == NULL || wide_to == NULL) {
    free(wide_from);
    free(wide_to);
    errno = EINVAL;
    return -1;
  }
  BOOL result = MoveFileExW(wide_from, wide_to, MOVEFILE_WRITE_THROUGH);
  free(wide_from);
  free(wide_to);
  if (!result) {
    win32_errno(GetLastError());
    return -1;
  }
  return 0;
}

int ta_fs_unlink(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) {
    errno = EINVAL;
    return -1;
  }
  int result = _wunlink(wide);
  free(wide);
  return result;
}

struct TaDir {
  HANDLE handle;
  WIN32_FIND_DATAW data;
  int first;
};

TaDir *ta_dir_open(const char *path) {
  size_t length = strlen(path);
  int needs_separator =
      length > 0 && path[length - 1] != '/' && path[length - 1] != '\\';
  char *pattern = malloc(length + (needs_separator ? 3u : 2u));
  if (pattern == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  memcpy(pattern, path, length);
  size_t at = length;
  if (needs_separator)
    pattern[at++] = '\\';
  pattern[at++] = '*';
  pattern[at] = '\0';

  wchar_t *base = path_to_wide(path);
  wchar_t *wide_pattern = path_to_wide(pattern);
  free(pattern);
  if (base == NULL || wide_pattern == NULL) {
    free(base);
    free(wide_pattern);
    errno = EINVAL;
    return NULL;
  }
  DWORD attributes = GetFileAttributesW(base);
  free(base);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    win32_errno(GetLastError());
    free(wide_pattern);
    return NULL;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    free(wide_pattern);
    errno = ENOTDIR;
    return NULL;
  }

  TaDir *dir = calloc(1, sizeof(*dir));
  if (dir == NULL) {
    free(wide_pattern);
    errno = ENOMEM;
    return NULL;
  }
  dir->first = 1;
  dir->handle = FindFirstFileW(wide_pattern, &dir->data);
  free(wide_pattern);
  if (dir->handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      dir->handle = INVALID_HANDLE_VALUE;
      return dir;
    }
    win32_errno(error);
    free(dir);
    return NULL;
  }
  return dir;
}

static void fill_find_stat(const WIN32_FIND_DATAW *data, TaFileStat *out) {
  memset(out, 0, sizeof(*out));
  out->symlink = (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  out->directory = (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out->regular = !out->directory && !out->symlink;
  out->mode = out->directory ? 0755u : 0644u;
  out->size = ((uint64_t)data->nFileSizeHigh << 32) | data->nFileSizeLow;
  out->mtime_ns = filetime_ns(data->ftLastWriteTime);
  out->ctime_ns = filetime_ns(data->ftCreationTime);
  out->mtime = (int64_t)(out->mtime_ns / 1000000000ULL);
}

int ta_dir_next(TaDir *dir, char **name, TaFileStat *info) {
  if (dir == NULL || name == NULL || info == NULL) {
    errno = EINVAL;
    return -1;
  }
  *name = NULL;
  if (dir->handle == INVALID_HANDLE_VALUE)
    return 0;
  for (;;) {
    if (dir->first) {
      dir->first = 0;
    } else if (!FindNextFileW(dir->handle, &dir->data)) {
      DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_FILES)
        return 0;
      win32_errno(error);
      return -1;
    }
    if (wcscmp(dir->data.cFileName, L".") == 0 ||
        wcscmp(dir->data.cFileName, L"..") == 0)
      continue;
    char *utf8 = ta_wide_to_utf8(dir->data.cFileName);
    if (utf8 == NULL) {
      errno = ENOMEM;
      return -1;
    }
    fill_find_stat(&dir->data, info);
    *name = utf8;
    return 1;
  }
}

void ta_dir_close(TaDir *dir) {
  if (dir == NULL)
    return;
  if (dir->handle != INVALID_HANDLE_VALUE)
    FindClose(dir->handle);
  free(dir);
}

#else /* POSIX */

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

long long ta_monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000L);
}

int ta_isatty(int fd) { return isatty(fd) != 0; }

char *ta_strdup(const char *text) {
  if (text == NULL)
    return NULL;
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (copy != NULL)
    memcpy(copy, text, length + 1);
  return copy;
}

int ta_strcasecmp(const char *a, const char *b) {
  const unsigned char *left = (const unsigned char *)a;
  const unsigned char *right = (const unsigned char *)b;
  for (;;) {
    unsigned char l = *left;
    unsigned char r = *right;
    if (l >= 'A' && l <= 'Z')
      l = (unsigned char)(l - 'A' + 'a');
    if (r >= 'A' && r <= 'Z')
      r = (unsigned char)(r - 'A' + 'a');
    if (l != r)
      return (int)l - (int)r;
    if (l == 0)
      return 0;
    ++left;
    ++right;
  }
}

static void fill_stat(const struct stat *info, TaFileStat *out) {
  memset(out, 0, sizeof(*out));
  out->regular = S_ISREG(info->st_mode) != 0;
  out->directory = S_ISDIR(info->st_mode) != 0;
  out->symlink = S_ISLNK(info->st_mode) != 0;
  out->mode = (unsigned int)(info->st_mode & 07777);
  out->size = (uint64_t)info->st_size;
  out->mtime = (int64_t)info->st_mtim.tv_sec;
  out->mtime_ns = (uint64_t)info->st_mtim.tv_sec * 1000000000ULL +
                  (uint64_t)info->st_mtim.tv_nsec;
  out->ctime_ns = (uint64_t)info->st_ctim.tv_sec * 1000000000ULL +
                  (uint64_t)info->st_ctim.tv_nsec;
  out->device = (uint64_t)info->st_dev;
  out->inode = (uint64_t)info->st_ino;
}

int ta_fs_lstat(const char *path, TaFileStat *out) {
  struct stat info;
  if (lstat(path, &info) != 0)
    return -1;
  fill_stat(&info, out);
  return 0;
}

int ta_fs_fstat(int fd, TaFileStat *out) {
  struct stat info;
  if (fstat(fd, &info) != 0)
    return -1;
  fill_stat(&info, out);
  return 0;
}

int ta_fs_same_file(const TaFileStat *a, const TaFileStat *b) {
  return a->device == b->device && a->inode == b->inode && a->size == b->size &&
         a->mode == b->mode && a->mtime_ns == b->mtime_ns &&
         a->ctime_ns == b->ctime_ns;
}

int ta_fs_open_read(const char *path) {
  return open(path, O_RDONLY | O_NONBLOCK);
}

int ta_fs_close(int fd) { return close(fd); }

ptrdiff_t ta_fs_read(int fd, void *buffer, size_t count) {
  return (ptrdiff_t)read(fd, buffer, count);
}

ptrdiff_t ta_fs_write(int fd, const void *buffer, size_t count) {
  return (ptrdiff_t)write(fd, buffer, count);
}

int ta_fs_sync(int fd) { return fsync(fd); }

int ta_fs_chmod(int fd, unsigned int mode) { return fchmod(fd, (mode_t)mode); }

int ta_fs_mkstemp(char *path_template) { return mkstemp(path_template); }

int ta_fs_rename(const char *from, const char *to) { return rename(from, to); }

int ta_fs_publish_noreplace(const char *from, const char *to) {
  return link(from, to);
}

int ta_fs_unlink(const char *path) { return unlink(path); }

struct TaDir {
  DIR *handle;
};

TaDir *ta_dir_open(const char *path) {
  DIR *handle = opendir(path);
  if (handle == NULL)
    return NULL;
  TaDir *dir = malloc(sizeof(*dir));
  if (dir == NULL) {
    closedir(handle);
    errno = ENOMEM;
    return NULL;
  }
  dir->handle = handle;
  return dir;
}

int ta_dir_next(TaDir *dir, char **name, TaFileStat *info) {
  if (dir == NULL || name == NULL || info == NULL) {
    errno = EINVAL;
    return -1;
  }
  *name = NULL;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir->handle);
    if (entry == NULL)
      return errno != 0 ? -1 : 0;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    struct stat stat_info;
    if (fstatat(dirfd(dir->handle), entry->d_name, &stat_info,
                AT_SYMLINK_NOFOLLOW) != 0)
      return -1;
    char *copy = ta_strdup(entry->d_name);
    if (copy == NULL) {
      errno = ENOMEM;
      return -1;
    }
    fill_stat(&stat_info, info);
    *name = copy;
    return 1;
  }
}

void ta_dir_close(TaDir *dir) {
  if (dir == NULL)
    return;
  closedir(dir->handle);
  free(dir);
}

#endif /* _WIN32 */
