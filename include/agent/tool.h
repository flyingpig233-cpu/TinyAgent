#ifndef TINY_AGENT_TOOL_H
#define TINY_AGENT_TOOL_H

#include <stddef.h>
#include <cjson/cJSON.h>
#include "agent/error.h"
#include "agent/status.h"

typedef struct ToolContext ToolContext;
typedef struct {
  char *content;
  int is_error; // 0 or 1
} ToolOutput;

// The callback receives borrowed arguments and a currently NULL context.
// If it supplies an output, it transfers ownership of the malloc-allocated
// output and content to the caller, including when returning an error.
// On success, output and content must be non-NULL. The content is sent to the
// model for either is_error value; a non-OK return aborts the agent run.
typedef AgentStatus (*ToolExecutionFn) (
  ToolContext *ctx,
  const cJSON *arguments,
  ToolOutput **output,
  AgentError *error
);

typedef struct {
  char *call_id;
  char *tool_name;
  char *arguments_json;
} ToolCall;

// Borrowed function definition for an LLM request. parameters_json must be a
// JSON Schema object. Set strict explicitly to get the same behavior in both
// OpenAI protocols; strict schemas must meet the API's schema requirements.
typedef struct {
  const char *name;
  const char *description; // Optional.
  const char *parameters_json;
  int strict; // 0 or 1.

  ToolExecutionFn fn;
} ToolDefinition;


typedef struct {
  // The registry owns this array but borrows its tool definitions.
  const ToolDefinition **tools;

  size_t count;
  size_t capacity;
} ToolRegistry;

// On success, the caller owns the registry. On failure, *registry is NULL.
int tool_registry_init(ToolRegistry **registry);
// Frees the registry and its array, but not the borrowed tool definitions.
int tool_registry_destory(ToolRegistry *registry);

// Returns a borrowed definition, or NULL if not found or an argument is NULL.
const ToolDefinition *tool_registry_find(const ToolRegistry *reg,
                                         const char *tool_name);

// Borrows the definition and its strings until the registry is destroyed.
// Names must be nonempty and unique; parameters_json and fn are required.
// The caller must keep the definition and its strings unchanged while registered.
int tool_registry_register(ToolRegistry *reg, const ToolDefinition *tool_def,
                           AgentError *error);

#endif // TINY_AGENT_TOOL_H
