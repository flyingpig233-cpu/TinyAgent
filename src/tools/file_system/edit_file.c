#define _POSIX_C_SOURCE 200809L
#include "agent/tools/file_system/edit_file.h"
#include "agent/tools/file_system/read_file.h"
#include "../internal.h"
#include <stdlib.h>
#include <string.h>

const ToolDefinition edit_file_tool = {
    .name = "edit_file", .description = EDIT_FILE_DESCRIPTION,
    .parameters_json = EDIT_FILE_PARAMETERS_JSON, .strict = 1, .mutating = 1, .fn = edit_file};

AgentStatus edit_file_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &edit_file_tool, error);
}

AgentStatus edit_file(ToolContext *ctx, const cJSON *args, ToolOutput **out, AgentError *error) {
  AgentStatus status = tool_begin(args, out, error);
  if (status != AGENT_OK) return status;
  const char *path = tool_string(args, "path"), *old = tool_string(args, "old_text");
  const char *replacement = tool_string(args, "new_text");
  if (path == NULL || *path == '\0' || old == NULL || *old == '\0' || replacement == NULL)
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "path, nonempty old_text and new_text are required");
  struct stat before;
  if (lstat(path, &before) != 0)
    return tool_fail(error, AGENT_ERR_IO, "Could not stat file to edit");
  if (!S_ISREG(before.st_mode))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "Only regular files may be edited; symlinks are rejected");
  ToolOutput *source = NULL;
  status = read_file(ctx, args, &source, error);
  if (status != AGENT_OK) return status;
  char *match = strstr(source->content, old);
  char *edited = NULL;
  size_t old_len = strlen(old), new_len = strlen(replacement);
  if (!tool_utf8((const unsigned char *)old, old_len)) {
    status = tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "old_text must be UTF-8 text");
    goto done;
  }
  if (match == NULL || strstr(match + 1, old) != NULL) {
    status = tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "old_text must match exactly once");
    goto done;
  }
  size_t length = strlen(source->content) - old_len;
  if (new_len > TOOL_FILE_LIMIT - length || !tool_utf8((const unsigned char *)replacement, new_len)) {
    status = tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "Edited content must be UTF-8 text up to 1 MiB");
    goto done;
  }
  edited = malloc(length + new_len + 1);
  if (edited == NULL) {
    status = tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate edited content");
    goto done;
  }
  size_t prefix = (size_t)(match - source->content);
  memcpy(edited, source->content, prefix);
  memcpy(edited + prefix, replacement, new_len);
  strcpy(edited + prefix + new_len, match + old_len);
  cJSON *json = cJSON_CreateObject();
  if (json == NULL || !cJSON_AddStringToObject(json, "path", path) ||
      !cJSON_AddNumberToObject(json, "replacements", 1) ||
      !cJSON_AddNumberToObject(json, "bytes_written", (double)(length + new_len))) {
    cJSON_Delete(json);
    status = tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate edit result");
    goto done;
  }
  status = tool_json(json, 0, out, error);
  cJSON_Delete(json);
  if (status == AGENT_OK) status = tool_atomic_write(path, edited, 0, &before, error);
done:
  free(edited);
  free(source->content);
  free(source);
  if (status != AGENT_OK && *out != NULL) { free((*out)->content); free(*out); *out = NULL; }
  return status;
}
