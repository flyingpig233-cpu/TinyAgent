#include "agent/agent.h"
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

static int malformed(void *ctx, const LLMRequest *request, LLMResponse **out,
                     AgentError *error) {
  (void)ctx;
  (void)request;
  (void)error;
  *out = calloc(1, sizeof(**out));
  if (*out == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  (*out)->tool_call_count = 1;
  return AGENT_OK;
}

static int failure(void *ctx, const LLMRequest *request, LLMResponse **out,
                   AgentError *error) {
  (void)request;
  (void)out;
  if (ctx != NULL)
    ERROR(error, "provider detail");
  return AGENT_ERR_HTTP_TIMEOUT;
}

int main(void) {
  AgentError error = {0};
  agent_error_clear(NULL);
  agent_error_set(NULL, NULL, NULL, NULL, 0);
  agent_error_set(&error, "detail", "test.c", "func", 42);
  CHECK(strcmp(error.message, "detail") == 0);
  CHECK(strcmp(error.file, "test.c") == 0 &&
        strcmp(error.function, "func") == 0 && error.line == 42);
  agent_error_set(&error, error.message, NULL, NULL, 0);
  CHECK(strcmp(error.message, "detail") == 0);
  char long_message[400];
  memset(long_message, 'a', sizeof(long_message) - 1);
  long_message[sizeof(long_message) - 1] = '\0';
  agent_error_set(&error, long_message, NULL, NULL, 0);
  CHECK(strlen(error.message) == sizeof(error.message) - 1);
  agent_error_clear(&error);
  CHECK(error.message[0] == '\0' && error.file == NULL && error.line == 0);
  agent_error_set(&error, NULL, NULL, NULL, 0);
  CHECK(error.message[0] == '\0');

  LLMProvider provider = {.ops = {.complete = malformed}};
  AgentConfig config = {.max_round = 1};
  Agent *agent = NULL;
  Context context = {0};
  Session session = {.context = &context};
  String output = {0};
  CHECK(context_init(&context) == AGENT_OK);
  CHECK(agent_create(&config, &provider, &agent) == AGENT_OK);
  CHECK(agent_run(agent, &session, "one", &output, &error) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(context.array->count == 1);
  CHECK(error.message[0] != '\0');
  provider.ops.complete = failure;
  CHECK(agent_run(agent, &session, "two", &output, &error) ==
        AGENT_ERR_HTTP_TIMEOUT);
  CHECK(strcmp(error.message, "LLM provider failed") == 0);
  provider.ctx = &provider;
  CHECK(agent_run(agent, &session, "three", &output, &error) ==
        AGENT_ERR_HTTP_TIMEOUT);
  CHECK(strcmp(error.message, "provider detail") == 0);
  CHECK(error.function != NULL && strcmp(error.function, "failure") == 0);
  agent_destroy(agent);
  CHECK(context_destory(&context) == AGENT_OK);
  string_destroy(&output);
  return 0;
}
