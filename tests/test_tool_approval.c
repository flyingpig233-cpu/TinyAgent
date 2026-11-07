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

static int g_executed;

static AgentStatus exec_tool(ToolContext *ctx, const cJSON *args,
                             ToolOutput **out, AgentError *error) {
  (void)ctx;
  (void)args;
  (void)error;
  g_executed++;
  *out = calloc(1, sizeof(**out));
  if (*out == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  (*out)->content = malloc(4);
  if ((*out)->content == NULL) {
    free(*out);
    *out = NULL;
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  memcpy((*out)->content, "ran", 4);
  return AGENT_OK;
}

typedef struct {
  int calls;
  int last_mutating;
  ToolDecision next;
  char last_tool[64];
} ApprovalLog;

static ToolDecision approve(void *userdata, const char *tool_name,
                            const char *arguments_json, int mutating) {
  ApprovalLog *log = userdata;
  (void)arguments_json;
  log->calls++;
  log->last_mutating = mutating;
  snprintf(log->last_tool, sizeof(log->last_tool), "%s",
           tool_name != NULL ? tool_name : "");
  return log->next;
}

/* Runs one agent turn against `responses`, wiring the given tool and approval
 * log. Returns the run status via *status and the tool-result message text via
 * *tool_message (may be left empty). */
static int run_case(LLMResponse *responses, size_t response_count,
                    ToolDefinition *tool, ApprovalLog *log, int use_approval,
                    int max_round, AgentStatus *status, char *tool_message,
                    size_t tool_message_cap) {
  const ToolDefinition *tools[] = {tool};
  ToolRegistry registry = {.tools = tools, .count = 1, .capacity = 1};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  AgentConfig config = {.max_round = max_round};
  if (use_approval) {
    config.on_tool_approval = approve;
    config.approval_userdata = log;
  }
  Agent *agent = NULL;
  String output = {0};
  AgentError error = {0};

  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, responses, response_count) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  *status = agent_run(agent, &session, "question", &output, &error);
  if (tool_message != NULL && tool_message_cap > 0) {
    tool_message[0] = '\0';
    for (size_t i = 0; i < context.array->count; ++i) {
      if (context.array->items[i].role == TOOL_MESSAGE) {
        snprintf(tool_message, tool_message_cap, "%s",
                 context.array->items[i].content != NULL
                     ? context.array->items[i].content
                     : "");
        break;
      }
    }
  }
  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

static int case_deny_mutating(void) {
  g_executed = 0;
  ToolCall call = {.call_id = "c1", .tool_name = "danger",
                   .arguments_json = "{}"};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "danger", .parameters_json = "{}",
                         .mutating = 1, .fn = exec_tool};
  ApprovalLog log = {.next = TOOL_DECISION_DENY};
  AgentStatus status = AGENT_OK;
  char message[256];

  CHECK(run_case(responses, 2, &tool, &log, 1, 2, &status, message,
                 sizeof(message)) == 0);
  CHECK(status == AGENT_OK);
  CHECK(g_executed == 0);     /* Denied call never ran. */
  CHECK(log.calls == 1);      /* Approval asked exactly once. */
  CHECK(log.last_mutating == 1);
  CHECK(strcmp(log.last_tool, "danger") == 0);
  CHECK(strstr(message, "denied") != NULL);
  return 0;
}

static int case_allow_mutating(void) {
  g_executed = 0;
  ToolCall call = {.call_id = "c1", .tool_name = "danger",
                   .arguments_json = "{}"};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "danger", .parameters_json = "{}",
                         .mutating = 1, .fn = exec_tool};
  ApprovalLog log = {.next = TOOL_DECISION_ALLOW};
  AgentStatus status = AGENT_OK;
  char message[256];

  CHECK(run_case(responses, 2, &tool, &log, 1, 2, &status, message,
                 sizeof(message)) == 0);
  CHECK(status == AGENT_OK);
  CHECK(g_executed == 1);
  CHECK(log.calls == 1);
  CHECK(strcmp(message, "ran") == 0);
  return 0;
}

static int case_read_only_tool_not_flagged(void) {
  g_executed = 0;
  ToolCall call = {.call_id = "c1", .tool_name = "safe",
                   .arguments_json = "{}"};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "safe", .parameters_json = "{}",
                         .mutating = 0, .fn = exec_tool};
  ApprovalLog log = {.next = TOOL_DECISION_ALLOW};
  AgentStatus status = AGENT_OK;

  CHECK(run_case(responses, 2, &tool, &log, 1, 2, &status, NULL, 0) == 0);
  CHECK(status == AGENT_OK);
  CHECK(g_executed == 1);
  CHECK(log.calls == 1);
  CHECK(log.last_mutating == 0); /* Host sees the flag and can skip prompting. */
  return 0;
}

static int case_allow_all_is_sticky(void) {
  g_executed = 0;
  ToolCall calls[] = {{.call_id = "c1", .tool_name = "danger",
                       .arguments_json = "{}"},
                      {.call_id = "c2", .tool_name = "danger",
                       .arguments_json = "{}"}};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = calls,
       .tool_call_count = 2},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "danger", .parameters_json = "{}",
                         .mutating = 1, .fn = exec_tool};
  ApprovalLog log = {.next = TOOL_DECISION_ALLOW_ALL};
  AgentStatus status = AGENT_OK;

  CHECK(run_case(responses, 2, &tool, &log, 1, 2, &status, NULL, 0) == 0);
  CHECK(status == AGENT_OK);
  CHECK(g_executed == 2); /* Both calls in the same round ran. */
  CHECK(log.calls == 1);  /* ALLOW_ALL short-circuits the second prompt. */
  return 0;
}

static int case_no_callback_allows_all(void) {
  g_executed = 0;
  ToolCall call = {.call_id = "c1", .tool_name = "danger",
                   .arguments_json = "{}"};
  LLMResponse responses[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = &call,
       .tool_call_count = 1},
      {.finish_reason = LLM_FINISH_STOP, .content = "done"}};
  ToolDefinition tool = {.name = "danger", .parameters_json = "{}",
                         .mutating = 1, .fn = exec_tool};
  ApprovalLog log = {0};
  AgentStatus status = AGENT_OK;

  CHECK(run_case(responses, 2, &tool, &log, 0, 2, &status, NULL, 0) == 0);
  CHECK(status == AGENT_OK);
  CHECK(g_executed == 1); /* NULL callback allows every call (default). */
  CHECK(log.calls == 0);
  return 0;
}

int main(void) {
  CHECK(case_deny_mutating() == 0);
  CHECK(case_allow_mutating() == 0);
  CHECK(case_read_only_tool_not_flagged() == 0);
  CHECK(case_allow_all_is_sticky() == 0);
  CHECK(case_no_callback_allows_all() == 0);
  return 0;
}
