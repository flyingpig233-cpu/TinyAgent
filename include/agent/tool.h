#ifndef TINY_AGENT_TOOL_H
#define TINY_AGENT_TOOL_H

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
} ToolDefinition;

#endif // TINY_AGENT_TOOL_H
