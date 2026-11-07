#include "agent/agent.h"
#include "agent/mock_llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static AgentStatus execute_tool(ToolContext *ctx, const cJSON *args,
                                ToolOutput **out, AgentError *error) {
  (void)ctx;
  (void)error;
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, "value");
  if (!cJSON_IsNumber(value) || value->valueint != 42)
    return AGENT_ERR_INVAILD_ARGUMENT;
  *out = calloc(1, sizeof(**out));
  if (*out == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  (*out)->content = malloc(7);
  if ((*out)->content == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  memcpy((*out)->content, "result", 7);
  return AGENT_OK;
}

static int exercise_tool_call(void) {
  ToolCall call = {.call_id = "call_1", .tool_name = "test",
                   .arguments_json = "{\"value\":42}"};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "test", .parameters_json = "{}",
                         .fn = execute_tool};
  const ToolDefinition *tools[] = {&tool};
  ToolRegistry registry = {.tools = tools, .count = 1, .capacity = 1};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  AgentConfig config = {.max_round = 2};
  Agent *agent = NULL;
  String output = {0};
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, responses, 2) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(agent_run(agent, &session, "question", &output, NULL) == AGENT_OK);
  CHECK(context.array->count == 4);
  CHECK(context.array->items[2].role == TOOL_MESSAGE);
  CHECK(strcmp(context.array->items[2].tool_call_id, "call_1") == 0);
  CHECK(strcmp(context.array->items[2].content, "result") == 0);
  CHECK(strcmp(string_c_str(&output), "done") == 0);
  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

static int exercise(LLMFinishReason reason, const char *content, int expected) {
  LLMResponse fixture = {.content = (char *)content, .finish_reason = reason};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  AgentConfig config = {.max_round = 2};
  Agent *agent = NULL;
  ToolRegistry registry = {0};
  String output = {0};
  AgentError error = {0};
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, &fixture, 1) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(string_assign(&output, "old") == AGENT_OK);
  CHECK(agent_run(agent, &session, "question", &output, &error) == expected);
  CHECK(context.array->count == 2);
  CHECK(strcmp(context.array->items[0].content, "question") == 0);
  CHECK(context.array->items[1].role == ASSISTANT_MESSAGE);
  CHECK(strcmp(string_c_str(&output),
               expected == AGENT_OK ? (content ? content : "") : "old") == 0);
  if (expected != AGENT_OK)
    CHECK(error.message[0] != '\0');
  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

int main(void) {
  CHECK(exercise_tool_call() == 0);
  CHECK(exercise(LLM_FINISH_STOP, "answer", AGENT_OK) == 0);
  CHECK(exercise(LLM_FINISH_STOP, NULL, AGENT_OK) == 0);
  CHECK(exercise(LLM_FINISH_LENGTH, "partial", AGENT_ERR_LLM_TRUNCATED) == 0);
  CHECK(exercise(LLM_FINISH_FILTERED, NULL, AGENT_ERR_LLM_FILTERED) == 0);
  CHECK(exercise(LLM_FINISH_REFUSED, NULL, AGENT_ERR_LLM_REFUSED) == 0);
  CHECK(exercise(LLM_FINISH_TOOL_CALLS, NULL, AGENT_ERR_LLM_RESPONSE) == 0);
  CHECK(exercise(LLM_FINISH_UNKNOWN, NULL, AGENT_ERR_LLM_RESPONSE) == 0);
  CHECK(agent_run(NULL, NULL, NULL, NULL, NULL) == AGENT_ERR_NULL_POINTER);
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  AgentConfig config = {.max_round = 0};
  Agent *agent = NULL;
  ToolRegistry registry = {0};
  String output = {0};
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, NULL, 0) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(agent_run(agent, &session, "x", &output, NULL) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(context.array->count == 0);
  config.max_round = 1;
  CHECK(agent_run(agent, &session, "x", &output, NULL) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(context.array->count == 1);
  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}
