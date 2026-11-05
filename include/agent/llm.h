#ifndef TINY_AGENT_LLM_H
#define TINY_AGENT_LLM_H

#include <stddef.h>

#include "agent/error.h"
#include "agent/tool.h"
typedef struct Context Context;

typedef enum {
    LLM_FINISH_UNKNOWN,
    LLM_FINISH_STOP,          // 正常完成
    LLM_FINISH_TOOL_CALLS,    // 请求调用工具
    LLM_FINISH_LENGTH,        // 达到输出长度限制
    LLM_FINISH_FILTERED        // 输出被过滤
} LLMFinishReason;

typedef struct LLMResponse {
    char *content;

    // For LLM responses that include tool calls
    ToolCall *tool_calls;
    size_t tool_call_count;

    LLMFinishReason finish_reason;

} LLMResponse;

typedef struct {
    const Context *context;
} LLMRequest;

typedef struct {
    int (*complete) (
        void *provider_ctx,
        const LLMRequest *request,
        LLMResponse **response,
        AgentError *error
    );

    int (*destory) (void *provider_ctx);
} LLMProviderOps;

typedef struct {
    LLMProviderOps ops;
    void *ctx;
} LLMProvider;


// On success, the caller owns *out and must call llm_response_destroy.
// Providers must allocate responses and their fields for llm_response_destroy.
// tool_call_count must not exceed the allocated array length; it must be zero
// when tool_calls is NULL. On failure, providers should leave *out NULL.
int llm_provider_complete(LLMProvider *provider, const LLMRequest *request, LLMResponse **out, AgentError *error);
void llm_response_destroy(LLMResponse *response);

int llm_provider_destory(LLMProvider *provider, AgentError *error);



#endif // TINY_AGENT_LLM_H