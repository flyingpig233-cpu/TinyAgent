#ifndef TINY_AGENT_CONTEXT_H
#define TINY_AGENT_CONTEXT_H

#include "agent/error.h"
#include "agent/tool.h"

#include <stddef.h>
typedef struct LLMResponse LLMResponse;

typedef enum {
  SYSTEM_MESSAGE,
  USER_MESSAGE,
  ASSISTANT_MESSAGE,
  TOOL_MESSAGE
} MessageRole;

typedef struct {
  MessageRole role;
  const char *content;

  // For LLM responses that include tool calls
  ToolCall *tool_calls;
  size_t tool_call_count;

  // For tool messages that include tool call information
  char *tool_call_id;
  char *tool_name;
} Message;

typedef struct {
  Message *items;
  size_t count;
  size_t capacity;
} MessageArrary;

typedef struct Context {
  MessageArrary *array;
} Context;

int context_init(Context *ctx);
int context_add_assistant_response(Context *ctx, const LLMResponse *response,
                                   AgentError *error);
int context_append(Context *ctx, const Message *message);
int context_destory(Context *ctx);

#endif // TINY_AGENT_CONTEXT_H
