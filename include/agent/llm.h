#ifndef TINY_AGENT_LLM_H
#define TINY_AGENT_LLM_H

#include <stddef.h>

#include "agent/continuation.h"
#include "agent/error.h"
#include "agent/tool.h"
typedef struct Context Context;

typedef enum {
  LLM_FINISH_UNKNOWN,
  LLM_FINISH_STOP,
  LLM_FINISH_TOOL_CALLS,
  LLM_FINISH_LENGTH,
  LLM_FINISH_FILTERED,
  LLM_FINISH_REFUSED
} LLMFinishReason;

typedef struct LLMResponse {
  char *content;

  // For LLM responses that include tool calls
  ToolCall *tool_calls;
  size_t tool_call_count;

  LLMFinishReason finish_reason;

  // Owned strings and history, released by llm_response_destroy.
  char *provider_status;
  char *provider_reason;
  char *refusal;
  LLMContinuation continuation;

} LLMResponse;

typedef struct {
  const Context *context;
  const ToolDefinition *const *tools; // Borrowed for the duration of complete().
  size_t tool_count;

  // When nonzero, providers should request incremental output and invoke
  // on_delta as text arrives. The final response must still be assembled and
  // returned normally, so tool calls and history are unaffected.
  int stream;
  void (*on_delta)(void *userdata, const char *text); // Borrowed text.
  void *delta_userdata;

  // Optional poll hook. Providers call it periodically during a request; a
  // nonzero return aborts the request with AGENT_ERR_CANCELLED. It may also be
  // used to drive progress UI, so it should be cheap.
  int (*is_cancelled)(void *userdata);
  void *cancel_userdata;
} LLMRequest;

typedef struct {
  int (*complete)(void *provider_ctx, const LLMRequest *request,
                  LLMResponse **response, AgentError *error);

  int (*destory)(void *provider_ctx);
} LLMProviderOps;

typedef struct {
  LLMProviderOps ops;
  void *ctx;
} LLMProvider;

// On success, the caller owns *out and must call llm_response_destroy.
// Providers must allocate responses and their fields for llm_response_destroy.
// tool_call_count must not exceed the allocated array length; it must be zero
// when tool_calls is NULL. On failure, providers should leave *out NULL.
int llm_provider_complete(LLMProvider *provider, const LLMRequest *request,
                          LLMResponse **out, AgentError *error);

void llm_response_destroy(LLMResponse *response);
int llm_provider_destory(LLMProvider *provider, AgentError *error);

#endif // TINY_AGENT_LLM_H
