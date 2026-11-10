#include "agent/tools/file_system/list_dir.h"
#include "../internal.h"
#include "agent/infra.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const ToolDefinition list_dir_tool = {
    .name = "list_dir",
    .description = LIST_DIR_DESCRIPTION,
    .parameters_json = LIST_DIR_PARAMETERS_JSON,
    .strict = 1,
    .mutating = 0,
    .fn = list_dir,
};

static const char *entry_type(const TaFileStat *info) {
  if (info->symlink)
    return "symlink";
  if (info->regular)
    return "file";
  if (info->directory)
    return "directory";
  return "unknown";
}

AgentStatus list_dir_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &list_dir_tool, error);
}

AgentStatus list_dir(ToolContext *ctx, const cJSON *arguments,
                     ToolOutput **output, AgentError *error) {
  (void)ctx;
  agent_error_clear(error);
  if (output == NULL || arguments == NULL) {
    AGENT_ERROR(error, "Arguments and output are required");
    return AGENT_ERR_NULL_POINTER;
  }
  *output = NULL;
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(arguments, "path");
  if (!cJSON_IsObject(arguments) || !cJSON_IsString(path) ||
      path->valuestring == NULL || path->valuestring[0] == '\0') {
    AGENT_ERROR(error, "path must be a nonempty string");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }

  TaDir *dir = ta_dir_open(path->valuestring);
  if (dir == NULL) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open directory: %s",
             strerror(errno));
    AGENT_ERROR(error, message);
    return AGENT_ERR_IO;
  }
  AgentStatus status = AGENT_OK;
  cJSON *entries = cJSON_CreateArray();
  if (entries == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    goto done;
  }
  for (;;) {
    char *name = NULL;
    TaFileStat info;
    int next = ta_dir_next(dir, &name, &info);
    if (next == 0)
      break;
    if (next < 0) {
      status = AGENT_ERR_IO;
      break;
    }
    if (!tool_utf8((const unsigned char *)name, strlen(name))) {
      free(name);
      status = AGENT_ERR_INVAILD_ARGUMENT;
      break;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || !cJSON_AddStringToObject(item, "name", name) ||
        !cJSON_AddStringToObject(item, "type", entry_type(&info)) ||
        !cJSON_AddNumberToObject(item, "size", (double)info.size) ||
        !cJSON_AddNumberToObject(item, "mode", (double)info.mode) ||
        !cJSON_AddNumberToObject(item, "modified", (double)info.mtime) ||
        !cJSON_AddItemToArray(entries, item)) {
      cJSON_Delete(item);
      free(name);
      status = AGENT_ERR_OUT_OF_MEMORY;
      break;
    }
    free(name);
  }
  if (status == AGENT_OK) {
    char *json = cJSON_PrintUnformatted(entries);
    ToolOutput *result = calloc(1, sizeof(*result));
    if (json == NULL || result == NULL ||
        full_copy(&result->content, json) != AGENT_OK) {
      free(result);
      status = AGENT_ERR_OUT_OF_MEMORY;
    } else {
      *output = result;
    }
    cJSON_free(json);
  }
done:
  cJSON_Delete(entries);
  ta_dir_close(dir);
  if (status != AGENT_OK) {
    if (*output != NULL) {
      free((*output)->content);
      free(*output);
      *output = NULL;
    }
    AGENT_ERROR(error, status == AGENT_ERR_IO
                           ? "Could not read directory entries"
                       : status == AGENT_ERR_INVAILD_ARGUMENT
                           ? "Directory entry name is not UTF-8 text"
                           : "Could not allocate directory listing");
  }
  return status;
}
