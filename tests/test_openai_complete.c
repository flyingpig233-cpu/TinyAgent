#include "agent/agent.h"
#include "agent/providers/openai.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "line %d: %s\n", __LINE__, #expr);                       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int test_tools(const char *base, OpenAIProtocol protocol) {
  char url[256];
  snprintf(url, sizeof(url), "%s/tools/", base);
  OpenAIConfig config = {
      .common = {.api_key = "secret", .base_url = url, .model = "test-model"},
      .protocol = protocol};
  LLMProvider *provider = NULL;
  CHECK(openai_provider_init(&provider, &config) == AGENT_OK);
  Context ctx = {0};
  CHECK(context_init(&ctx) == AGENT_OK);
  Message user = {.role = USER_MESSAGE, .content = "tool please"};
  CHECK(context_append(&ctx, &user) == AGENT_OK);
  ToolDefinition tool = {.name = "weather",
                         .parameters_json = "{\"type\":\"object\"}",
                         .strict = 0};
  LLMRequest request = {.context = &ctx, .tools = &tool, .tool_count = 1};
  LLMResponse *response = NULL;
  AgentError error = {0};
  CHECK(llm_provider_complete(provider, &request, &response, &error) ==
        AGENT_OK);
  CHECK(response->finish_reason == LLM_FINISH_TOOL_CALLS &&
        response->tool_call_count == 1);
  CHECK(!strcmp(response->tool_calls[0].call_id, "call_1"));
  CHECK(context_add_assistant_response(&ctx, response, &error) == AGENT_OK);
  llm_response_destroy(response);
  Message result = {
      .role = TOOL_MESSAGE, .content = "sunny", .tool_call_id = "call_1"};
  CHECK(context_append(&ctx, &result) == AGENT_OK);
  CHECK(llm_provider_complete(provider, &request, &response, &error) ==
        AGENT_OK);
  CHECK(response->finish_reason == LLM_FINISH_STOP &&
        !strcmp(response->content, "forecast"));
  llm_response_destroy(response);
  CHECK(context_destory(&ctx) == AGENT_OK);
  CHECK(openai_provider_destory(provider) == AGENT_OK);
  return 0;
}

static int test_sessions(const char *base, OpenAIProtocol protocol) {
  char url[256];
  snprintf(url, sizeof(url), "%s/sessions", base);
  OpenAIConfig config = {
      .common = {.api_key = "secret", .base_url = url, .model = "test-model"},
      .protocol = protocol};
  LLMProvider *provider = NULL;
  CHECK(openai_provider_init(&provider, &config) == AGENT_OK);
  AgentConfig settings = config_default();
  Agent *agent = NULL;
  CHECK(agent_create(&settings, provider, &agent) == AGENT_OK);
  Session *a = NULL, *b = NULL;
  CHECK(session_init(&a, "a", "session-a") == AGENT_OK);
  CHECK(session_init(&b, "b", "session-b") == AGENT_OK);
  AgentError error = {0};
  String output = {0};
  CHECK(agent_run(agent, a, "hello", &output, &error) == AGENT_OK);
  CHECK(!strcmp(string_c_str(&output), "hello back"));
  CHECK(agent_run(agent, b, "hello", &output, &error) == AGENT_OK);
  CHECK(agent_run(agent, a, "followup", &output, &error) == AGENT_OK);
  CHECK(!strcmp(string_c_str(&output), "hello back"));
  string_destroy(&output);
  agent_destroy(agent);
  CHECK(session_destory(a) == AGENT_OK);
  CHECK(session_destory(b) == AGENT_OK);
  CHECK(openai_provider_destory(provider) == AGENT_OK);
  return 0;
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  char url[256];
  Message message = {.role = USER_MESSAGE, .content = "hello"};
  MessageArrary array = {.items = &message, .count = 1, .capacity = 1};
  Context context = {.array = &array};
  LLMRequest request = {.context = &context};
  const char *paths[] = {"ok",  "http_error", "invalid",
                         "nul", "override",   "failed"};
  const int expected[] = {AGENT_OK,
                          AGENT_ERR_HTTP,
                          AGENT_ERR_LLM_RESPONSE,
                          AGENT_ERR_LLM_RESPONSE,
                          AGENT_OK,
                          AGENT_ERR_LLM_RESPONSE};
  for (int p = 0; p < 2; ++p) {
    OpenAIProtocol protocol =
        p ? PROTOCOL_RESPONSES : PROTOCOL_CHAT_COMPLETIONS;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
      if (i == 5 && !p)
        continue;
      // Multiple trailing slashes must not produce duplicate separators.
      snprintf(url, sizeof(url), "%s/%s///", argv[1], paths[i]);
      OpenAIConfig config = {.common = {.api_key = "secret",
                                        .base_url = url,
                                        .model = "test-model"},
                             .protocol = protocol};
      if (i == 4) {
        snprintf(url, sizeof(url), "%s/custom/%s", argv[1],
                 p ? "responses" : "chat/completions");
        config.common.base_url = "http://unused.invalid";
        config.endpoint_url = url;
      }
      LLMProvider *provider = NULL;
      CHECK(openai_provider_init(&provider, &config) == AGENT_OK);
      url[0] = 'X'; // Configuration and endpoint must have been copied.
      AgentError error = {0};
      LLMResponse *response = NULL;
      int status = llm_provider_complete(provider, &request, &response, &error);
      if (status != expected[i])
        fprintf(stderr, "protocol %d, case %s: %d %s\n", p, paths[i], status,
                error.message);
      CHECK(status == expected[i]);
      if (status == AGENT_OK) {
        CHECK(response != NULL && !strcmp(response->content, "hello back") &&
              !error.message[0]);
      } else {
        CHECK(response == NULL && error.message[0] != '\0');
      }
      if (i == 1)
        CHECK(strstr(error.message, "401") &&
              strstr(error.message, "invalid_api_key"));
      if (i == 5)
        CHECK(strstr(error.message, "server_error") &&
              strstr(error.message, "try again"));
      llm_response_destroy(response);
      CHECK(openai_provider_destory(provider) == AGENT_OK);
    }
    CHECK(test_tools(argv[1], protocol) == 0);
    CHECK(test_sessions(argv[1], protocol) == 0);
  }
  OpenAIConfig config = {.common = {.api_key = "secret\r\nX-Bad: value",
                                    .base_url = argv[1],
                                    .model = "model"}};
  LLMProvider *provider = NULL;
  CHECK(openai_provider_init(&provider, &config) == AGENT_OK);
  LLMResponse *response = NULL;
  CHECK(llm_provider_complete(provider, &request, &response, NULL) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(response == NULL);
  CHECK(openai_provider_destory(provider) == AGENT_OK);
  return 0;
}
