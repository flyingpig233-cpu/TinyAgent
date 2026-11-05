#include <stdio.h>
#include "agent/agent.h"
#include "agent/mock_llm.h"

int main(void) {
    Agent *agent = NULL;
    Session *session = NULL;
    AgentConfig config = config_default();

    LLMResponse replies[] = {
        {.content = "你好", .finish_reason = LLM_FINISH_STOP}
    };
    LLMProvider provider = {0};

    mock_llm_init(&provider, replies, 1);
    session_init(&session, "Test Session", "You are a helpful assistant");

    agent_create(&config, &provider, &agent);

    String str;
    string_init(&str);

    AgentError err;
    agent_run(agent, session, "Hello", &str, &err);

    printf("Output: %s\n", string_c_str(&str));

    string_destroy(&str);
    session_destory(session);
    agent_destroy(agent);

    return 0;
}