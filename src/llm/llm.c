#include "agent/llm.h"
#include <stdlib.h>

void llm_response_destroy(LLMResponse *response) {
  if (response == NULL)
    return;
  free(response->content);
  for (size_t i = 0;
       response->tool_calls != NULL && i < response->tool_call_count; ++i) {
    free(response->tool_calls[i].call_id);
    free(response->tool_calls[i].tool_name);
    free(response->tool_calls[i].arguments_json);
  }
  free(response->tool_calls);
  free(response);
}

int llm_provider_complete(LLMProvider *provider, const LLMRequest *request,
                          LLMResponse **response, AgentError *error) {
  if (provider == NULL || response == NULL)
    return AGENT_ERR_NULL_POINTER;
  *response = NULL;
  if (provider->ops.complete == NULL)
    return AGENT_ERR_INVAILD_ARGUMENT;
  return provider->ops.complete(provider->ctx, request, response, error);
}

int llm_provider_destory(LLMProvider *provider, AgentError *error) {
  (void)error;
  if (provider == NULL)
    return AGENT_ERR_NULL_POINTER;
  if (provider->ops.destory == NULL)
    return AGENT_ERR_INVAILD_ARGUMENT;
  int status = provider->ops.destory(provider->ctx);
  if (status == AGENT_OK)
    *provider = (LLMProvider){0};
  return status;
}
