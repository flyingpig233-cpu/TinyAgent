#include "agent/context.h"
#include "agent/llm.h"

enum { DEFAULT_CAPACITY = 19 };
#include "agent/infra.h"
#include <stdint.h>
#include <stdlib.h>

int context_init(Context *ctx) {
  if (ctx == NULL)
    return AGENT_ERR_NULL_POINTER;
  ctx->array = (MessageArrary *)malloc(sizeof(MessageArrary));
  if (ctx->array == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  ctx->array->capacity = DEFAULT_CAPACITY;
  ctx->array->count = 0;
  ctx->array->items = (Message *)malloc(DEFAULT_CAPACITY * sizeof(Message));
  if (ctx->array->items == NULL) {
    free(ctx->array);
    ctx->array = NULL;
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  return AGENT_OK;
}

int context_add_assistant_response(Context *ctx, const LLMResponse *response,
                                   AgentError *error) {
  (void)error;
  if (ctx == NULL || response == NULL) {
    return AGENT_ERR_NULL_POINTER;
  }

  Message message = {
      .role = ASSISTANT_MESSAGE,
      .content = response->content,
      .refusal = response->refusal,
      .tool_calls = response->tool_calls,
      .tool_call_count = response->tool_call_count,
      .continuation = response->continuation,
  };

  return context_append(ctx, &message);
}

static int context_copy_optional_string(char **dest, const char *src) {
  *dest = NULL;
  return src == NULL ? AGENT_OK : full_copy(dest, src);
}

static void message_clear(Message *message) {
  free((void *)message->content);
  free((void *)message->refusal);
  free(message->tool_call_id);
  free(message->tool_name);
  free(message->continuation.items_json);
  for (size_t i = 0; i < message->tool_call_count; ++i) {
    free(message->tool_calls[i].arguments_json);
    free(message->tool_calls[i].call_id);
    free(message->tool_calls[i].tool_name);
  }
  free(message->tool_calls);
}

int context_append(Context *ctx, const Message *message) {
  if (ctx == NULL || message == NULL || ctx->array == NULL)
    return AGENT_ERR_NULL_POINTER;

  MessageArrary *array = ctx->array;
  if (array->count > array->capacity ||
      (array->capacity > 0 && array->items == NULL) ||
      message->role < SYSTEM_MESSAGE || message->role > TOOL_MESSAGE ||
      (message->tool_call_count > 0 && message->tool_calls == NULL) ||
      (message->refusal != NULL && message->role != ASSISTANT_MESSAGE) ||
      (message->continuation.format != LLM_CONTINUATION_NONE &&
       message->continuation.format != LLM_CONTINUATION_OPENAI_RESPONSES) ||
      ((message->continuation.items_json != NULL) !=
       (message->continuation.format != LLM_CONTINUATION_NONE)) ||
      (message->continuation.format != LLM_CONTINUATION_NONE &&
       message->role != ASSISTANT_MESSAGE))
    return AGENT_ERR_INVAILD_ARGUMENT;

  const size_t max_capacity = SIZE_MAX / sizeof(Message);
  if (array->capacity > max_capacity || array->count == max_capacity ||
      message->tool_call_count > SIZE_MAX / sizeof(ToolCall))
    return AGENT_ERR_OUT_OF_MEMORY;

  Message copy = {0};
  copy.role = message->role;
  char *content = NULL;
  AgentStatus status = context_copy_optional_string(&content, message->content);
  copy.content = content;
  if (status != AGENT_OK)
    goto fail;

  char *refusal = NULL;
  status = context_copy_optional_string(&refusal, message->refusal);
  copy.refusal = refusal;
  if (status != AGENT_OK)
    goto fail;

  copy.continuation.format = message->continuation.format;
  status = context_copy_optional_string(&copy.continuation.items_json,
                                        message->continuation.items_json);
  if (status != AGENT_OK)
    goto fail;
  status =
      context_copy_optional_string(&copy.tool_call_id, message->tool_call_id);
  if (status != AGENT_OK)
    goto fail;
  status = context_copy_optional_string(&copy.tool_name, message->tool_name);
  if (status != AGENT_OK)
    goto fail;

  if (message->tool_call_count > 0) {
    copy.tool_calls = calloc(message->tool_call_count, sizeof(ToolCall));
    if (copy.tool_calls == NULL) {
      status = AGENT_ERR_OUT_OF_MEMORY;
      goto fail;
    }
    copy.tool_call_count = message->tool_call_count;
    for (size_t i = 0; i < copy.tool_call_count; ++i) {
      ToolCall *dest = &copy.tool_calls[i];
      const ToolCall *src = &message->tool_calls[i];
      status = context_copy_optional_string(&dest->arguments_json,
                                            src->arguments_json);
      if (status != AGENT_OK)
        goto fail;
      status = context_copy_optional_string(&dest->call_id, src->call_id);
      if (status != AGENT_OK)
        goto fail;
      status = context_copy_optional_string(&dest->tool_name, src->tool_name);
      if (status != AGENT_OK)
        goto fail;
    }
  }

  // Copy before realloc: message may point into array->items.
  if (array->count == array->capacity) {
    size_t capacity = array->capacity == 0 ? DEFAULT_CAPACITY
                                           : (array->capacity > max_capacity / 2
                                                  ? max_capacity
                                                  : array->capacity * 2);
    Message *items = realloc(array->items, capacity * sizeof(Message));
    if (items == NULL) {
      status = AGENT_ERR_OUT_OF_MEMORY;
      goto fail;
    }
    array->items = items;
    array->capacity = capacity;
  }

  array->items[array->count++] = copy;
  return AGENT_OK;

fail:
  message_clear(&copy);
  return status;
}

int context_destory(Context *ctx) {
  if (ctx == NULL)
    return AGENT_ERR_NULL_POINTER;
  if (ctx->array == NULL)
    return AGENT_OK;
  for (size_t i = 0; i < ctx->array->count; ++i)
    message_clear(&ctx->array->items[i]);
  free(ctx->array->items);
  free(ctx->array);
  ctx->array = NULL;
  return AGENT_OK;
}
