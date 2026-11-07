#include "agent/tool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static AgentStatus execute(ToolContext *ctx, const cJSON *args,
                           ToolOutput **output, AgentError *error) {
  (void)ctx; (void)args; (void)output; (void)error;
  return AGENT_OK;
}

int main(void) {
  ToolRegistry *registry = NULL;
  AgentError error;
  assert(tool_registry_init(&registry) == AGENT_OK);
  assert(tool_registry_find(registry, "missing") == NULL);
  assert(tool_registry_find(NULL, "missing") == NULL);
  assert(tool_registry_find(registry, NULL) == NULL);
  assert(tool_registry_register(NULL, NULL, &error) == AGENT_ERR_NULL_POINTER);
  assert(error.message[0] != '\0');

  ToolDefinition tools[11] = {0};
  char names[11][16];
  for (size_t i = 0; i < 11; ++i) {
    snprintf(names[i], sizeof(names[i]), "tool_%zu", i);
    tools[i] = (ToolDefinition){.name = names[i], .parameters_json = "{}",
                                .fn = execute};
    assert(tool_registry_register(registry, &tools[i], &error) == AGENT_OK);
    assert(error.message[0] == '\0');
  }
  assert(registry->count == 11 && registry->capacity >= 11);
  for (size_t i = 0; i < 11; ++i)
    assert(tool_registry_find(registry, names[i]) == &tools[i]);
  assert(tool_registry_find(registry, "TOOL_0") == NULL);
  assert(tool_registry_register(registry, &tools[0], &error) ==
         AGENT_ERR_INVAILD_ARGUMENT);
  ToolDefinition invalid = {.name = "invalid"};
  assert(tool_registry_register(registry, &invalid, &error) ==
         AGENT_ERR_INVAILD_ARGUMENT);
  assert(registry->count == 11);
  assert(tool_registry_destory(registry) == AGENT_OK);
  assert(strcmp(tools[0].name, "tool_0") == 0);
  return 0;
}
