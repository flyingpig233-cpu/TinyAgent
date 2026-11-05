#include <stdio.h>
#include <string.h>
#include "agent/agent.h"
#include "agent/mock_llm.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

static int exercise(LLMFinishReason reason, const char *content, int expected) {
    LLMResponse fixture = {.content = (char *)content, .finish_reason = reason};
    LLMProvider provider = {0};
    Context context = {0};
    Session session = {.context = &context};
    AgentConfig config = {.max_round = 2};
    Agent *agent = NULL;
    String output = {0};
    AgentError error = {0};
    CHECK(context_init(&context) == AGENT_OK);
    CHECK(mock_llm_init(&provider, &fixture, 1) == AGENT_OK);
    CHECK(agent_create(&config, &provider, &agent) == AGENT_OK);
    CHECK(string_assign(&output, "old") == AGENT_OK);
    CHECK(agent_run(agent, &session, "question", &output, &error) == expected);
    CHECK(context.array->count == 2);
    CHECK(strcmp(context.array->items[0].content, "question") == 0);
    CHECK(context.array->items[1].role == ASSISTANT_MESSAGE);
    CHECK(strcmp(string_c_str(&output), expected == AGENT_OK ? (content ? content : "") : "old") == 0);
    if (expected != AGENT_OK) CHECK(error.message[0] != '\0');
    agent_destroy(agent);
    CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
    CHECK(context_destory(&context) == AGENT_OK);
    string_destroy(&output);
    return 0;
}

int main(void) {
    CHECK(exercise(LLM_FINISH_STOP, "answer", AGENT_OK) == 0);
    CHECK(exercise(LLM_FINISH_STOP, NULL, AGENT_OK) == 0);
    CHECK(exercise(LLM_FINISH_LENGTH, "partial", AGENT_ERR_LLM_TRUNCATED) == 0);
    CHECK(exercise(LLM_FINISH_FILTERED, NULL, AGENT_ERR_LLM_FILTERED) == 0);
    CHECK(exercise(LLM_FINISH_TOOL_CALLS, NULL, AGENT_ERR_TOOL_UNSUPPORTED) == 0);
    CHECK(exercise(LLM_FINISH_UNKNOWN, NULL, AGENT_ERR_LLM_RESPONSE) == 0);
    CHECK(agent_run(NULL, NULL, NULL, NULL, NULL) == AGENT_ERR_NULL_POINTER);
    LLMProvider provider = {0};
    Context context = {0};
    Session session = {.context = &context};
    AgentConfig config = {.max_round = 0};
    Agent *agent = NULL;
    String output = {0};
    CHECK(context_init(&context) == AGENT_OK);
    CHECK(mock_llm_init(&provider, NULL, 0) == AGENT_OK);
    CHECK(agent_create(&config, &provider, &agent) == AGENT_OK);
    CHECK(agent_run(agent, &session, "x", &output, NULL) == AGENT_ERR_INVAILD_ARGUMENT);
    CHECK(context.array->count == 0);
    config.max_round = 1;
    CHECK(agent_run(agent, &session, "x", &output, NULL) == AGENT_ERR_INVAILD_ARGUMENT);
    CHECK(context.array->count == 1);
    agent_destroy(agent);
    CHECK(llm_provider_destory(&provider, NULL) == AGENT_OK);
    CHECK(context_destory(&context) == AGENT_OK);
    string_destroy(&output);
    return 0;
}
