#ifndef TINY_AGENT_CONTINUATION_H
#define TINY_AGENT_CONTINUATION_H

typedef enum {
  LLM_CONTINUATION_NONE = 0,
  LLM_CONTINUATION_OPENAI_RESPONSES
} LLMContinuationFormat;

// Protocol-specific history, separate from the normalized text/tool calls.
// Responses and stored context messages own items_json. Input messages borrow
// it.
typedef struct {
  LLMContinuationFormat format;
  char *items_json;
} LLMContinuation;

#endif
