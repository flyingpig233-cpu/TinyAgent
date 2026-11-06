#include "agent/context.h"
#include "agent/providers/openai.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  char url[256];
  Message message = {.role = USER_MESSAGE, .content = "hello"};
  MessageArrary array = {.items = &message, .count = 1, .capacity = 1};
  Context context = {.array = &array};
  LLMRequest request = {.context = &context};
  const char *paths[] = {"ok", "http_error", "invalid", "nul"};
  const int expected[] = {AGENT_OK, AGENT_ERR_HTTP, AGENT_ERR_LLM_RESPONSE,
                          AGENT_ERR_LLM_RESPONSE};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    snprintf(url, sizeof(url), "%s/%s", argv[1], paths[i]);
    ProviderConfig config = {
        .api_key = "secret", .base_url = url, .model = "test-model"};
    LLMProvider *provider = NULL;
    if (openai_provider_init(&provider, &config) != AGENT_OK)
      return 2;
    AgentError error = {0};
    LLMResponse *response = NULL;
    int status = llm_provider_complete(provider, &request, &response, &error);
    if (status != expected[i]) {
      fprintf(stderr, "case %s: %d %s\n", paths[i], status, error.message);
      return 3;
    }
    if (status == AGENT_OK) {
      if (response == NULL || strcmp(response->content, "hello back") ||
          error.message[0])
        return 4;
    } else if (response != NULL || error.message[0] == '\0')
      return 5;
    if (i == 1 && strstr(error.message, "401") == NULL)
      return 6;
    llm_response_destroy(response);
    if (openai_provider_destory(provider) != AGENT_OK)
      return 7;
  }
  ProviderConfig config = {.api_key = "secret\r\nX-Bad: value",
                           .base_url = argv[1],
                           .model = "model"};
  LLMProvider *provider = NULL;
  if (openai_provider_init(&provider, &config) != AGENT_OK)
    return 8;
  LLMResponse *response = NULL;
  if (llm_provider_complete(provider, &request, &response, NULL) !=
          AGENT_ERR_INVAILD_ARGUMENT ||
      response != NULL)
    return 9;
  return openai_provider_destory(provider) == AGENT_OK ? 0 : 10;
}
