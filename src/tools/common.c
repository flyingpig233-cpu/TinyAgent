#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

AgentStatus tool_fail(AgentError *error, AgentStatus status,
                      const char *message) {
  AGENT_ERROR(error, message);
  return status;
}

AgentStatus tool_begin(const cJSON *args, ToolOutput **out, AgentError *error) {
  agent_error_clear(error);
  if (out != NULL)
    *out = NULL;
  if (args == NULL || out == NULL)
    return tool_fail(error, AGENT_ERR_NULL_POINTER,
                     "Arguments and output are required");
  if (!cJSON_IsObject(args))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT,
                     "Arguments must be an object");
  return AGENT_OK;
}

const char *tool_string(const cJSON *args, const char *key) {
  return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, key));
}

int tool_optional_string(const cJSON *args, const char *key,
                         const char **value) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
  *value = NULL;
  if (item == NULL || cJSON_IsNull(item))
    return 1;
  *value = cJSON_GetStringValue(item);
  return *value != NULL && **value != '\0';
}

int tool_integer(const cJSON *args, const char *key, int fallback, int min,
                 int max, int *value) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
  *value = fallback;
  if (item == NULL || cJSON_IsNull(item))
    return 1;
  if (!cJSON_IsNumber(item) ||
      !(item->valuedouble >= min && item->valuedouble <= max))
    return 0;
  *value = (int)item->valuedouble;
  return item->valuedouble == *value;
}

AgentStatus tool_json(cJSON *json, int is_error, ToolOutput **out,
                      AgentError *error) {
  char *text = json == NULL ? NULL : cJSON_PrintUnformatted(json);
  ToolOutput *result = calloc(1, sizeof(*result));
  if (text == NULL || result == NULL ||
      full_copy(&result->content, text) != AGENT_OK) {
    cJSON_free(text);
    free(result);
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY,
                     "Could not allocate tool output");
  }
  cJSON_free(text);
  result->is_error = is_error;
  *out = result;
  return AGENT_OK;
}

int tool_utf8(const unsigned char *s, size_t length) {
  for (size_t i = 0; i < length;) {
    unsigned char c = s[i++];
    if (c == 0)
      return 0;
    if (c < 0x80)
      continue;
    unsigned int code;
    size_t n;
    if (c >= 0xc2 && c <= 0xdf) {
      code = c & 0x1f;
      n = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
      code = c & 0x0f;
      n = 2;
    } else if (c >= 0xf0 && c <= 0xf4) {
      code = c & 0x07;
      n = 3;
    } else
      return 0;
    if (n > length - i)
      return 0;
    for (size_t j = 0; j < n; ++j) {
      unsigned char next = s[i++];
      if ((next & 0xc0) != 0x80)
        return 0;
      code = (code << 6) | (next & 0x3f);
    }
    if ((n == 2 && code < 0x800) || (n == 3 && code < 0x10000) ||
        (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
      return 0;
  }
  return 1;
}

AgentStatus tool_atomic_write(const char *path, const char *content, int create,
                              const TaFileStat *expected, AgentError *error) {
  TaFileStat before;
  if (create) {
    if (ta_fs_lstat(path, &before) == 0 || errno != ENOENT)
      return tool_fail(error, AGENT_ERR_IO, "Create requires an absent path");
  } else {
    if (ta_fs_lstat(path, &before) != 0)
      return tool_fail(error, AGENT_ERR_IO,
                       "Overwrite requires an existing file");
    if (!before.regular)
      return tool_fail(
          error, AGENT_ERR_INVAILD_ARGUMENT,
          "Only regular files may be replaced; symlinks are rejected");
    if (expected != NULL && !ta_fs_same_file(expected, &before))
      return tool_fail(error, AGENT_ERR_IO, "File changed while being edited");
  }
  const char *slash = strrchr(path, '/');
#if defined(_WIN32)
  const char *backslash = strrchr(path, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash))
    slash = backslash;
#endif
  size_t prefix = slash == NULL ? 0 : (size_t)(slash - path + 1);
  const char suffix[] = ".tiny-agent-XXXXXX";
  char *temp = malloc(prefix + sizeof(suffix));
  if (temp == NULL)
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY,
                     "Could not allocate temporary path");
  memcpy(temp, path, prefix);
  memcpy(temp + prefix, suffix, sizeof(suffix));
  int fd = ta_fs_mkstemp(temp);
  AgentStatus status = AGENT_OK;
  if (fd < 0) {
    free(temp);
    return tool_fail(error, AGENT_ERR_IO,
                     "Could not create temporary file in parent directory");
  }
  size_t length = strlen(content), offset = 0;
  while (offset < length) {
    ptrdiff_t n = ta_fs_write(fd, content + offset, length - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      status = AGENT_ERR_IO;
      break;
    }
    offset += (size_t)n;
  }
  if (!create && ta_fs_chmod(fd, before.mode & 0777) != 0)
    status = AGENT_ERR_IO;
  if (status == AGENT_OK && ta_fs_sync(fd) != 0)
    status = AGENT_ERR_IO;
  if (ta_fs_close(fd) != 0)
    status = AGENT_ERR_IO;
  if (status == AGENT_OK && !create) {
    TaFileStat current;
    if (ta_fs_lstat(path, &current) != 0 || !ta_fs_same_file(&before, &current))
      status = AGENT_ERR_IO;
  }
  if (status == AGENT_OK) {
    if (create ? ta_fs_publish_noreplace(temp, path) != 0
               : ta_fs_rename(temp, path) != 0)
      status = AGENT_ERR_IO;
  }
  ta_fs_unlink(temp);
  free(temp);
  if (status != AGENT_OK)
    return tool_fail(error, status,
                     "Could not commit file; destination may have changed");
  return AGENT_OK;
}
