#include "agent/tool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { REGISTRY_DEFAULT_CAPACITY = 10 };

int tool_registry_init(ToolRegistry **registry) {
  if (registry == NULL)
    return AGENT_ERR_NULL_POINTER;
  *registry = NULL;

  ToolRegistry *reg = calloc(1, sizeof(*reg));
  if (reg == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  reg->tools = calloc(REGISTRY_DEFAULT_CAPACITY, sizeof(*reg->tools));
  if (reg->tools == NULL) {
    free(reg);
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  reg->capacity = REGISTRY_DEFAULT_CAPACITY;
  *registry = reg;
  return AGENT_OK;
}


int tool_registry_register(ToolRegistry *reg, const ToolDefinition *tool_def, AgentError *error) {
  agent_error_clear(error);
  if (reg == NULL || tool_def == NULL) {
    AGENT_ERROR(error, "Registry and tool definition are required");
    return AGENT_ERR_NULL_POINTER;
  }
  if (tool_def->name == NULL || tool_def->name[0] == '\0' ||
      tool_def->parameters_json == NULL || tool_def->fn == NULL ||
      (tool_def->strict != 0 && tool_def->strict != 1) ||
      (tool_def->mutating != 0 && tool_def->mutating != 1)) {
    AGENT_ERROR(error, "Invalid tool definition");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  for (size_t i = 0; i < reg->count; ++i) {
    if (strcmp(reg->tools[i]->name, tool_def->name) == 0) {
      AGENT_ERROR(error, "Tool name is already registered");
      return AGENT_ERR_INVAILD_ARGUMENT;
    }
  }
  if (reg->count == reg->capacity) {
    size_t max_capacity = SIZE_MAX / sizeof(*reg->tools);
    if (reg->capacity >= max_capacity) {
      AGENT_ERROR(error, "Tool registry capacity exceeded");
      return AGENT_ERR_OUT_OF_MEMORY;
    }
    size_t capacity = reg->capacity == 0 ? REGISTRY_DEFAULT_CAPACITY :
        (reg->capacity > max_capacity / 2 ? max_capacity : reg->capacity * 2);
    const ToolDefinition **tools = realloc(reg->tools, capacity * sizeof(*tools));
    if (tools == NULL) {
      AGENT_ERROR(error, "Failed to grow tool registry");
      return AGENT_ERR_OUT_OF_MEMORY;
    }
    reg->tools = tools;
    reg->capacity = capacity;
  }
  reg->tools[reg->count++] = tool_def;
  return AGENT_OK;
}

const ToolDefinition *tool_registry_find(const ToolRegistry *reg,
                                         const char *tool_name) {
  if (reg == NULL || tool_name == NULL)
    return NULL;
  for (size_t i = 0; i < reg->count; ++i) {
    if (strcmp(reg->tools[i]->name, tool_name) == 0)
      return reg->tools[i];
  }
  return NULL;
}

int tool_registry_destory(ToolRegistry *registry) {
  if (registry == NULL)
    return AGENT_OK;
  free(registry->tools);
  free(registry);
  return AGENT_OK;
}
