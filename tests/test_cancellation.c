#define _POSIX_C_SOURCE 200809L

#include "agent/agent.h"
#include "agent/context.h"
#include "agent/mock_llm.h"
#include "agent/tools/run_command.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int g_cancel;

static int cancel_hook(void *userdata) {
  (void)userdata;
  return g_cancel;
}

static long elapsed_ms(struct timespec start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long)(now.tv_sec - start.tv_sec) * 1000L +
         (now.tv_nsec - start.tv_nsec) / 1000000L;
}

/* A cancelled agent run must stop before it ever consults the provider. */
static int agent_cancel_test(void) {
  LLMProvider provider = {0};
  ToolRegistry registry = {0};
  Context context = {0};
  Session session = {.context = &context};
  Agent *agent = NULL;
  AgentConfig config = {
      .max_round = 5, .is_cancelled = cancel_hook, .cancel_userdata = NULL};
  AgentError error = {0};
  String output = {0};

  g_cancel = 1;
  CHECK(mock_llm_init(&provider, NULL, 0) == AGENT_OK);
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  int status = agent_run(agent, &session, "hello", &output, &error);
  CHECK(status == AGENT_ERR_CANCELLED);
  CHECK(strstr(error.message, "cancel") != NULL);
  CHECK(output.length == 0);

  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  g_cancel = 0;
  return 0;
}

/* A cancelled tool subprocess must be killed promptly, not run to completion.
 */
static int run_command_cancel_test(void) {
  cJSON *args = cJSON_Parse("{\"command\":\"sleep 5\"}");
  CHECK(args != NULL);
  ToolOutput *output = NULL;
  AgentError error = {0};
  ToolContext ctx = {.is_cancelled = cancel_hook, .userdata = NULL};

  g_cancel = 1;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  AgentStatus status = run_command(&ctx, args, &output, &error);
  long took = elapsed_ms(start);
  CHECK(status == AGENT_ERR_CANCELLED);
  CHECK(output == NULL);
  CHECK(took < 2000);

  cJSON_Delete(args);
  g_cancel = 0;
  return 0;
}

typedef struct {
  int mode; /* 0: before tools, 1: during a tool, 2: between tools. */
  int cancel;
  int calls;
  int results;
  int errors[2];
} CancelState;

static int state_cancelled(void *userdata) {
  return ((CancelState *)userdata)->cancel;
}

static void cancel_event(void *userdata, const AgentEvent *event) {
  CancelState *state = userdata;
  if (event->type == AGENT_EVENT_LLM_RESPONSE && state->mode == 0)
    state->cancel = 1;
  if (event->type == AGENT_EVENT_TOOL_RESULT && state->results < 2)
    state->errors[state->results++] = event->tool_is_error;
}

static AgentStatus cancel_tool(ToolContext *ctx, const cJSON *args,
                               ToolOutput **out, AgentError *error) {
  (void)args;
  (void)error;
  CancelState *state = ctx->userdata;
  ++state->calls;
  if (state->mode == 1)
    return AGENT_ERR_CANCELLED;
  *out = calloc(1, sizeof(**out));
  if (*out == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  int status = full_copy(&(*out)->content, "completed");
  state->cancel = 1;
  return status;
}

/* Cancellation must leave every announced tool call paired with a result,
 * without running pending tools or discarding already completed results. */
static int tool_history_cancel_test(int mode) {
  CancelState state = {.mode = mode};
  ToolCall calls[] = {{"c1", "test", "{}"}, {"c2", "test", "{}"}};
  LLMResponse fixtures[] = {
      {.finish_reason = LLM_FINISH_TOOL_CALLS, .tool_calls = calls,
       .tool_call_count = 2},
      {.finish_reason = LLM_FINISH_STOP, .content = "resumed"}};
  ToolDefinition tool = {.name = "test", .parameters_json = "{}",
                         .fn = cancel_tool};
  const ToolDefinition *tools[] = {&tool};
  ToolRegistry registry = {.tools = tools, .count = 1, .capacity = 1};
  LLMProvider provider = {0};
  Context context = {0};
  Session session = {.context = &context};
  AgentConfig config = {.max_round = 2,
                        .is_cancelled = state_cancelled,
                        .cancel_userdata = &state,
                        .on_event = cancel_event,
                        .userdata = &state};
  Agent *agent = NULL;
  String output = {0};
  AgentError error = {0};
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(mock_llm_init(&provider, fixtures, 2) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &registry, &agent) == AGENT_OK);
  CHECK(string_assign(&output, "old") == AGENT_OK);
  CHECK(agent_run(agent, &session, "go", &output, &error) == AGENT_ERR_CANCELLED);
  CHECK(strcmp(string_c_str(&output), "old") == 0);
  CHECK(state.calls == (mode == 0 ? 0 : 1));
  CHECK(state.results == 2 && state.errors[1] == 1);
  CHECK(state.errors[0] == (mode == 2 ? 0 : 1));
  CHECK(context.array->count == 4);
  for (size_t i = 0; i < 2; ++i) {
    const Message *result = &context.array->items[i + 2];
    CHECK(result->role == TOOL_MESSAGE);
    CHECK(strcmp(result->tool_call_id, calls[i].call_id) == 0);
  }
  if (mode == 2)
    CHECK(strcmp(context.array->items[2].content, "completed") == 0);
  state.cancel = 0;
  state.mode = 3;
  CHECK(agent_run(agent, &session, "again", &output, &error) == AGENT_OK);
  CHECK(strcmp(string_c_str(&output), "resumed") == 0);
  agent_destroy(agent);
  CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}

int main(void) {
  if (agent_cancel_test() != 0)
    return 1;
  if (run_command_cancel_test() != 0)
    return 1;
  for (int mode = 0; mode < 3; ++mode)
    if (tool_history_cancel_test(mode) != 0)
      return 1;
  return 0;
}
