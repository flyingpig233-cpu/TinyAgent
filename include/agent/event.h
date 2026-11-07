#ifndef TINY_AGENT_EVENT_H
#define TINY_AGENT_EVENT_H

#include "agent/llm.h"
#include "agent/status.h"

#include <stddef.h>

// Progress notifications emitted synchronously by agent_run().
typedef enum {
  AGENT_EVENT_RUN_START = 0,
  AGENT_EVENT_LLM_RESPONSE,
  AGENT_EVENT_TOOL_CALL,
  AGENT_EVENT_TOOL_RESULT,
  AGENT_EVENT_RUN_END
} AgentEventType;

// All string fields are borrowed from the agent and are only valid for the
// duration of the callback. Copy anything that must outlive the call.
typedef struct {
  AgentEventType type;

  // 1-based LLM round for response/tool events; 0 for run-level events.
  int round;

  // RUN_START: the user's input.
  // RUN_END: the final output on success, NULL on failure.
  const char *content;

  // LLM_RESPONSE: normalized text, finish reason and number of tool calls.
  LLMFinishReason finish_reason;
  size_t tool_call_count;

  // TOOL_CALL / TOOL_RESULT: tool identity.
  const char *tool_name;
  const char *call_id;

  // TOOL_CALL: raw JSON arguments string.
  const char *arguments_json;

  // TOOL_RESULT: output content (may be NULL) and its is_error flag.
  const char *tool_output;
  int tool_is_error;

  // RUN_END: final status and error message (NULL when no error is available).
  AgentStatus status;
  const char *error_message;
} AgentEvent;

// Called synchronously during agent_run(). Must not call back into the same
// agent. A NULL callback disables event delivery.
typedef void (*AgentEventCallback)(void *userdata, const AgentEvent *event);

#endif // TINY_AGENT_EVENT_H
