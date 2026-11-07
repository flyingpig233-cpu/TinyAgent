#define _POSIX_C_SOURCE 200809L
#include "agent/tools/file_system/list_dir.h"
#include "agent/infra.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const ToolDefinition list_dir_tool = {
    .name = "list_dir",
    .description = LIST_DIR_DESCRIPTION,
    .parameters_json = LIST_DIR_PARAMETERS_JSON,
    .strict = 1,
    .fn = list_dir,
};

static const char *entry_type(mode_t mode) {
  if (S_ISREG(mode)) return "file";
  if (S_ISDIR(mode)) return "directory";
  if (S_ISLNK(mode)) return "symlink";
  if (S_ISFIFO(mode)) return "fifo";
  if (S_ISSOCK(mode)) return "socket";
  if (S_ISCHR(mode)) return "character_device";
  if (S_ISBLK(mode)) return "block_device";
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
    ERROR(error, "Arguments and output are required");
    return AGENT_ERR_NULL_POINTER;
  }
  *output = NULL;
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(arguments, "path");
  if (!cJSON_IsObject(arguments) || !cJSON_IsString(path) ||
      path->valuestring == NULL || path->valuestring[0] == '\0') {
    ERROR(error, "path must be a nonempty string");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }

  DIR *dir = opendir(path->valuestring);
  if (dir == NULL) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open directory: %s", strerror(errno));
    ERROR(error, message);
    return AGENT_ERR_IO;
  }
  AgentStatus status = AGENT_OK;
  cJSON *entries = cJSON_CreateArray();
  if (entries == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    goto done;
  }
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) status = AGENT_ERR_IO;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    struct stat info;
    if (fstatat(dirfd(dir), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      status = AGENT_ERR_IO;
      break;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL ||
        !cJSON_AddStringToObject(item, "name", entry->d_name) ||
        !cJSON_AddStringToObject(item, "type", entry_type(info.st_mode)) ||
        !cJSON_AddNumberToObject(item, "size", (double)info.st_size) ||
        !cJSON_AddNumberToObject(item, "mode", (double)(info.st_mode & 07777)) ||
        !cJSON_AddNumberToObject(item, "modified", (double)info.st_mtime) ||
        !cJSON_AddItemToArray(entries, item)) {
      cJSON_Delete(item);
      status = AGENT_ERR_OUT_OF_MEMORY;
      break;
    }
  }
  if (status == AGENT_OK) {
    char *json = cJSON_PrintUnformatted(entries);
    ToolOutput *result = calloc(1, sizeof(*result));
    if (json == NULL || result == NULL || full_copy(&result->content, json) != AGENT_OK) {
      free(result);
      status = AGENT_ERR_OUT_OF_MEMORY;
    } else {
      *output = result;
    }
    cJSON_free(json);
  }
done:
  cJSON_Delete(entries);
  if (closedir(dir) != 0 && status == AGENT_OK) status = AGENT_ERR_IO;
  if (status != AGENT_OK) {
    if (*output != NULL) {
      free((*output)->content);
      free(*output);
      *output = NULL;
    }
    ERROR(error, status == AGENT_ERR_IO ? "Could not read directory entries" :
                                         "Could not allocate directory listing");
  }
  return status;
}
