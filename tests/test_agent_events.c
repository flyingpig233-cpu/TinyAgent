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

enum { MAX_EVENTS = 16, TEXT_CAP = 256 };

typedef struct {
  int calls;
  int count;
  AgentEventType type[MAX_EVENTS];
  int round[MAX_EVENTS];
  char content[MAX_EVENTS][TEXT_CAP];
  LLMFinishReason finish_reason[MAX_EVENTS];
  size_t tool_call_count[MAX_EVENTS];
  char tool_name[MAX_EVENTS][TEXT_CAP];
  char call_id[MAX_EVENTS][TEXT_CAP];
  char arguments_json[MAX_EVENTS][TEXT_CAP];
  char tool_output[MAX_EVENTS][TEXT_CAP];
  int tool_is_error[MAX_EVENTS];
  AgentStatus status[MAX_EVENTS];
  char error_message[MAX_EVENTS][TEXT_CAP];
} EventLog;

static void copy_text(char *dest, const char *src) {
  if (src == NULL) {
    dest[0] = '\0';
    return;
  }
  snprintf(dest, TEXT_CAP, "%s", src);
}

static void record_event(void *userdata, const AgentEvent *event) {
  EventLog *log = userdata;
  log->calls++;
  if (log->count >= MAX_EVENTS)
    return;
  int i = log->count++;
  log->type[i] = event->type;
  log->round[i] = event->round;
  copy_text(log->content[i], event->content);
  log->finish_reason[i] = event->finish_reason;
  log->tool_call_count[i] = event->tool_call_count;
  copy_text(log->tool_name[i], event->tool_name);
  copy_text(log->call_id[i], event->call_id);
  copy_text(log->arguments_json[i], event->arguments_json);
  copy_text(log->tool_output[i], event->tool_output);
  log->tool_is_error[i] = event->tool_is_error;
  log->status[i] = event->status;
  copy_text(log->error_message[i], event->error_message);
}

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
  if ((*out)->content == NULL) {
    free(*out);
    *out = NULL;
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  memcpy((*out)->content, "result", 7);
  return AGENT_OK;
}

static int exercise_tool_events(void) {
  ToolCall call = {.call_id = "call_1",
                   .tool_name = "test",
                   .arguments_json = "{\"value\":42}"};
  LLMResponse responses[] = {
      {.content = "let me check",
       .finish_reason = LLM_FINISH_TOOL_CALLS,
       .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {
      .name = "test", .parameters_json = "{}", .fn = execute_tool};
  const ToolDefinition *tools[] = {&tool};
  ToolRegistry registry = {.tools = tools, .count = 1, .capacity = 1};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  EventLog log = {0};
  AgentConfig config = {
      .max_round = 2, .on_event = record_event, .userdata = &log};
  Agent *agent = NULL;
  String output = {0};

  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, responses, 2) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(agent_run(agent, &session, "question", &output, NULL) == AGENT_OK);

  CHECK(log.calls == 6);
  CHECK(log.count == 6);

  CHECK(log.type[0] == AGENT_EVENT_RUN_START);
  CHECK(log.round[0] == 0);
  CHECK(strcmp(log.content[0], "question") == 0);

  CHECK(log.type[1] == AGENT_EVENT_LLM_RESPONSE);
  CHECK(log.round[1] == 1);
  CHECK(log.finish_reason[1] == LLM_FINISH_TOOL_CALLS);
  CHECK(log.tool_call_count[1] == 1);
  CHECK(strcmp(log.content[1], "let me check") == 0);

  CHECK(log.type[2] == AGENT_EVENT_TOOL_CALL);
  CHECK(log.round[2] == 1);
  CHECK(strcmp(log.tool_name[2], "test") == 0);
  CHECK(strcmp(log.call_id[2], "call_1") == 0);
  CHECK(strcmp(log.arguments_json[2], "{\"value\":42}") == 0);

  CHECK(log.type[3] == AGENT_EVENT_TOOL_RESULT);
  CHECK(log.round[3] == 1);
  CHECK(strcmp(log.tool_name[3], "test") == 0);
  CHECK(strcmp(log.call_id[3], "call_1") == 0);
  CHECK(strcmp(log.tool_output[3], "result") == 0);
  CHECK(log.tool_is_error[3] == 0);

  CHECK(log.type[4] == AGENT_EVENT_LLM_RESPONSE);
  CHECK(log.round[4] == 2);
  CHECK(log.finish_reason[4] == LLM_FINISH_STOP);
  CHECK(strcmp(log.content[4], "done") == 0);

  CHECK(log.type[5] == AGENT_EVENT_RUN_END);
  CHECK(log.status[5] == AGENT_OK);
  CHECK(strcmp(log.content[5], "done") == 0);
  CHECK(log.error_message[5][0] == '\0');

  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

static int exercise_error_events(void) {
  LLMResponse fixture = {.content = "partial",
                         .finish_reason = LLM_FINISH_LENGTH};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  ToolRegistry registry = {0};
  EventLog log = {0};
  AgentConfig config = {
      .max_round = 2, .on_event = record_event, .userdata = &log};
  Agent *agent = NULL;
  String output = {0};
  AgentError error = {0};

  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, &fixture, 1) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(agent_run(agent, &session, "question", &output, &error) ==
        AGENT_ERR_LLM_TRUNCATED);

  CHECK(log.calls == 3);
  CHECK(log.count == 3);
  CHECK(log.type[0] == AGENT_EVENT_RUN_START);
  CHECK(log.type[1] == AGENT_EVENT_LLM_RESPONSE);
  CHECK(log.finish_reason[1] == LLM_FINISH_LENGTH);
  CHECK(log.type[2] == AGENT_EVENT_RUN_END);
  CHECK(log.status[2] == AGENT_ERR_LLM_TRUNCATED);
  CHECK(log.content[2][0] == '\0');
  CHECK(log.error_message[2][0] != '\0');

  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

int main(void) {
  CHECK(exercise_tool_events() == 0);
  CHECK(exercise_error_events() == 0);
  return 0;
}
