#include "agent/tool.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static AgentStatus execute(ToolContext *ctx, const cJSON *args,
                           ToolOutput **output, AgentError *error) {
  (void)ctx;
  (void)args;
  (void)output;
  (void)error;
  return AGENT_OK;
}

int main(void) {
  ToolRegistry *registry = NULL;
  AgentError error;
  CHECK(tool_registry_init(&registry) == AGENT_OK);
  CHECK(tool_registry_find(registry, "missing") == NULL);
  CHECK(tool_registry_find(NULL, "missing") == NULL);
  CHECK(tool_registry_find(registry, NULL) == NULL);
  CHECK(tool_registry_register(NULL, NULL, &error) == AGENT_ERR_NULL_POINTER);
  CHECK(error.message[0] != '\0');

  ToolDefinition tools[11] = {0};
  char names[11][16];
  for (size_t i = 0; i < 11; ++i) {
    snprintf(names[i], sizeof(names[i]), "tool_%zu", i);
    tools[i].name = names[i];
    tools[i].parameters_json = "{}";
    tools[i].fn = execute;
    CHECK(tool_registry_register(registry, &tools[i], &error) == AGENT_OK);
    CHECK(error.message[0] == '\0');
  }
  CHECK(registry->count == 11 && registry->capacity >= 11);
  for (size_t i = 0; i < 11; ++i)
    CHECK(tool_registry_find(registry, names[i]) == &tools[i]);
  CHECK(tool_registry_find(registry, "TOOL_0") == NULL);
  CHECK(tool_registry_register(registry, &tools[0], &error) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  ToolDefinition invalid = {.name = "invalid"};
  CHECK(tool_registry_register(registry, &invalid, &error) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(registry->count == 11);
  CHECK(tool_registry_destory(registry) == AGENT_OK);
  CHECK(strcmp(tools[0].name, "tool_0") == 0);
  return 0;
}
