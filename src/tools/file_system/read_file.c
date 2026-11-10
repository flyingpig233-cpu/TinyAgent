#include "agent/tools/file_system/read_file.h"
#include "../internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { READ_FILE_MAX_BYTES = 1024 * 1024 };

const ToolDefinition read_file_tool = {.name = "read_file",
                                       .description = READ_FILE_DESCRIPTION,
                                       .parameters_json =
                                           READ_FILE_PARAMETERS_JSON,
                                       .strict = 1,
                                       .mutating = 0,
                                       .fn = read_file};

AgentStatus read_file_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &read_file_tool, error);
}

static int valid_utf8(const unsigned char *s, size_t length) {
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

AgentStatus read_file(ToolContext *ctx, const cJSON *arguments,
                      ToolOutput **output, AgentError *error) {
  (void)ctx;
  agent_error_clear(error);
  if (output != NULL)
    *output = NULL;
  if (arguments == NULL || output == NULL) {
    AGENT_ERROR(error, "Arguments and output are required");
    return AGENT_ERR_NULL_POINTER;
  }
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(arguments, "path");
  if (!cJSON_IsObject(arguments) || !cJSON_IsString(path) ||
      path->valuestring == NULL || path->valuestring[0] == '\0') {
    AGENT_ERROR(error, "path must be a nonempty string");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  int fd = ta_fs_open_read(path->valuestring);
  if (fd < 0) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open file: %s",
             strerror(errno));
    AGENT_ERROR(error, message);
    return AGENT_ERR_IO;
  }
  AgentStatus status = AGENT_OK;
  const char *message = NULL;
  TaFileStat info;
  char *content = NULL;
  ToolOutput *result = NULL;
  if (ta_fs_fstat(fd, &info) != 0) {
    status = AGENT_ERR_IO;
    message = "Could not stat file";
    goto done;
  }
  if (!info.regular) {
    status = AGENT_ERR_INVAILD_ARGUMENT;
    message = "Path is not a regular file";
    goto done;
  }
  if (info.size > (uint64_t)READ_FILE_MAX_BYTES) {
    status = AGENT_ERR_INVAILD_ARGUMENT;
    message = "File exceeds 1 MiB limit";
    goto done;
  }
  content = malloc((size_t)READ_FILE_MAX_BYTES + 2);
  if (content == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    message = "Could not allocate file contents";
    goto done;
  }
  size_t length = 0;
  while (length < (size_t)READ_FILE_MAX_BYTES + 1) {
    ptrdiff_t count = ta_fs_read(fd, content + length,
                                 (size_t)READ_FILE_MAX_BYTES + 1 - length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      status = AGENT_ERR_IO;
      message = "Could not read file";
      goto done;
    }
    if (count == 0)
      break;
    length += (size_t)count;
  }
  if (length > (size_t)READ_FILE_MAX_BYTES) {
    status = AGENT_ERR_INVAILD_ARGUMENT;
    message = "File exceeds 1 MiB limit";
    goto done;
  }
  if (!valid_utf8((const unsigned char *)content, length)) {
    status = AGENT_ERR_INVAILD_ARGUMENT;
    message = "File is not UTF-8 text or contains NUL bytes";
    goto done;
  }
  content[length] = '\0';
  result = calloc(1, sizeof(*result));
  if (result == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    message = "Could not allocate tool output";
    goto done;
  }
  result->content = content;
  content = NULL;
done:
  if (ta_fs_close(fd) != 0 && status == AGENT_OK) {
    status = AGENT_ERR_IO;
    message = "Could not close file";
  }
  free(content);
  if (status != AGENT_OK) {
    if (result != NULL) {
      free(result->content);
      free(result);
    }
    AGENT_ERROR(error, message);
  } else {
    *output = result;
  }
  return status;
}
