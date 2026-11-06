#include "agent/mock_llm.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  char text[] = "hello";
  char arguments[] = "{\"x\":1}";
  ToolCall calls[] = {{"call-1", "lookup", arguments}};
  LLMResponse fixtures[] = {{.content = text},
                            {.tool_calls = calls, .tool_call_count = 1}};
  LLMProvider provider = {0};
  CHECK(mock_llm_init(&provider, fixtures, 2) == AGENT_OK);
  text[0] = 'X';
  arguments[2] = 'y';
  LLMResponse *response = NULL;
  CHECK(llm_provider_complete(&provider, NULL, &response, NULL) == AGENT_OK);
  CHECK(strcmp(response->content, "hello") == 0);
  llm_response_destroy(response);
  CHECK(llm_provider_complete(&provider, NULL, &response, NULL) == AGENT_OK);
  CHECK(response->tool_call_count == 1);
  CHECK(strcmp(response->tool_calls[0].call_id, "call-1") == 0);
  CHECK(strcmp(response->tool_calls[0].tool_name, "lookup") == 0);
  CHECK(strcmp(response->tool_calls[0].arguments_json, "{\"x\":1}") == 0);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(strcmp(response->tool_calls[0].tool_name, "lookup") == 0);
  llm_response_destroy(response);
  CHECK(mock_llm_init(&provider, NULL, 0) == AGENT_OK);
  response = (LLMResponse *)&provider;
  CHECK(llm_provider_complete(&provider, NULL, &response, NULL) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(response == NULL);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  LLMResponse invalid = {.tool_call_count = 1};
  CHECK(mock_llm_init(&provider, &invalid, 1) == AGENT_ERR_INVAILD_ARGUMENT);
  return 0;
}
