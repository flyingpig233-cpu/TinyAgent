#define _POSIX_C_SOURCE 200809L
#include "agent/tools/file_system/list_dir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void) {
  char directory[] = "/tmp/tiny-agent-list-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  char file[256], subdir[256], link[256];
  snprintf(file, sizeof(file), "%s/.hidden", directory);
  snprintf(subdir, sizeof(subdir), "%s/subdir", directory);
  snprintf(link, sizeof(link), "%s/link", directory);
  FILE *f = fopen(file, "w");
  CHECK(f != NULL && fputs("abc", f) >= 0 && fclose(f) == 0);
  CHECK(mkdir(subdir, 0700) == 0);
  CHECK(symlink("missing", link) == 0);
  cJSON *args = cJSON_CreateObject();
  CHECK(args != NULL && cJSON_AddStringToObject(args, "path", directory));
  ToolOutput *output = NULL;
  AgentError error = {0};
  CHECK(list_dir(NULL, args, &output, &error) == AGENT_OK);
  CHECK(output != NULL && output->is_error == 0);
  cJSON *items = cJSON_Parse(output->content);
  CHECK(cJSON_IsArray(items) && cJSON_GetArraySize(items) == 3);
  int found = 0;
  cJSON *item;
  cJSON_ArrayForEach(item, items) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    CHECK(cJSON_IsString(name) && cJSON_IsString(type));
    if (strcmp(name->valuestring, ".hidden") == 0) {
      CHECK(strcmp(type->valuestring, "file") == 0);
      CHECK(cJSON_GetObjectItemCaseSensitive(item, "size")->valueint == 3);
      found |= 1;
    } else if (strcmp(name->valuestring, "subdir") == 0) {
      CHECK(strcmp(type->valuestring, "directory") == 0);
      found |= 2;
    } else if (strcmp(name->valuestring, "link") == 0) {
      CHECK(strcmp(type->valuestring, "symlink") == 0);
      found |= 4;
    }
  }
  CHECK(found == 7);
  cJSON_Delete(items);
  free(output->content);
  free(output);
  cJSON_Delete(args);
  args = cJSON_Parse("{\"path\":\"/nonexistent-tiny-agent-directory\"}");
  CHECK(list_dir(NULL, args, &output, &error) == AGENT_ERR_IO);
  CHECK(output == NULL && error.message[0] != '\0');
  cJSON_Delete(args);
  CHECK(unlink(link) == 0 && unlink(file) == 0 && rmdir(subdir) == 0 && rmdir(directory) == 0);
  return 0;
}
