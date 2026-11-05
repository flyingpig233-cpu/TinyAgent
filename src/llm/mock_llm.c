#include "agent/mock_llm.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LLMResponse **responses;
    size_t count;
    size_t next;
} MockLLM;

static int copy_string(char **out, const char *source) {
    *out = NULL;
    if (source == NULL) return AGENT_OK;
    size_t len = strlen(source);
    *out = malloc(len + 1);
    if (*out == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    memcpy(*out, source, len + 1);
    return AGENT_OK;
}

static int copy_response(const LLMResponse *source, LLMResponse **out) {
    *out = NULL;
    if (source->tool_call_count && source->tool_calls == NULL)
        return AGENT_ERR_INVAILD_ARGUMENT;
    if (source->tool_call_count > SIZE_MAX / sizeof(ToolCall))
        return AGENT_ERR_OUT_OF_MEMORY;
    LLMResponse *copy = calloc(1, sizeof(*copy));
    if (copy == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    int status = copy_string(&copy->content, source->content);
    if (status != AGENT_OK) goto fail;
    copy->finish_reason = source->finish_reason;
    if (source->tool_call_count) {
        copy->tool_calls = calloc(source->tool_call_count, sizeof(*copy->tool_calls));
        if (copy->tool_calls == NULL) { status = AGENT_ERR_OUT_OF_MEMORY; goto fail; }
        copy->tool_call_count = source->tool_call_count;
        for (size_t i = 0; i < copy->tool_call_count; ++i) {
            ToolCall *dest = &copy->tool_calls[i];
            const ToolCall *src = &source->tool_calls[i];
            status = copy_string(&dest->call_id, src->call_id);
            if (status != AGENT_OK) goto fail;
            status = copy_string(&dest->tool_name, src->tool_name);
            if (status != AGENT_OK) goto fail;
            status = copy_string(&dest->arguments_json, src->arguments_json);
            if (status != AGENT_OK) goto fail;
        }
    }
    *out = copy;
    return AGENT_OK;
fail:
    llm_response_destroy(copy);
    return status;
}

static int mock_complete(void *ctx, const LLMRequest *request,
                         LLMResponse **response, AgentError *error) {
    (void)request;
    (void)error;
    MockLLM *mock = ctx;
    if (mock->next == mock->count) return AGENT_ERR_INVAILD_ARGUMENT;
    *response = mock->responses[mock->next];
    mock->responses[mock->next++] = NULL;
    return AGENT_OK;
}

static int mock_destroy(void *ctx) {
    MockLLM *mock = ctx;
    for (size_t i = 0; i < mock->count; ++i)
        llm_response_destroy(mock->responses[i]);
    free(mock->responses);
    free(mock);
    return AGENT_OK;
}

int mock_llm_init(LLMProvider *provider, const LLMResponse *responses, size_t count) {
    if (provider == NULL || (count && responses == NULL)) return AGENT_ERR_NULL_POINTER;
    if (count > SIZE_MAX / sizeof(LLMResponse *)) return AGENT_ERR_OUT_OF_MEMORY;
    MockLLM *mock = calloc(1, sizeof(*mock));
    if (mock == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    if (count) {
        mock->responses = calloc(count, sizeof(*mock->responses));
        if (mock->responses == NULL) { free(mock); return AGENT_ERR_OUT_OF_MEMORY; }
    }
    mock->count = count;
    for (size_t i = 0; i < count; ++i) {
        int status = copy_response(&responses[i], &mock->responses[i]);
        if (status != AGENT_OK) { mock_destroy(mock); return status; }
    }
    *provider = (LLMProvider){.ops = {mock_complete, mock_destroy}, .ctx = mock};
    return AGENT_OK;
}
