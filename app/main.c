#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "agent/mock_llm.h"
#include "agent/providers/openai.h"

int main(void) {
  Agent *agent = NULL;
  Session *session = NULL;
  AgentConfig config = config_default();

  LLMProvider *provider = {0};
  const char *api_key = getenv("DEEPSEEK_API_KEY");
  if (api_key == NULL) {
    return 0;
  }
  ProviderConfig provider_config = {
      .api_key = api_key,
      .base_url = "https://api.deepseek.com/chat/completions",
      .model = "deepseek-flash"};

  openai_provider_init(&provider, &provider_config);

  session_init(&session, "Test Session", "You are a helpful assistant");

  agent_create(&config, provider, &agent);

  String msg;
  string_init(&msg);

  AgentError err;
  char user_input[512];
  printf("init successfully\n");
  for (;;) {
    printf("Input: ");
    fflush(stdout);

    if (fgets(user_input, sizeof(user_input), stdin) == NULL) {
      break;
    }

    user_input[strcspn(user_input, "\r\n")] = '\0';

    if (strcmp(user_input, "/exit") == 0) {
      break;
    }

    int status = agent_run(agent, session, user_input, &msg, &err);
    if (status != AGENT_OK) {
      fprintf(stderr, "failed to get response from LLM：%s(error code: %d)\n", err.message, status);
      continue;
    }

    printf("Output: %s\n", string_c_str(&msg));
  }

  string_destroy(&msg);
  session_destory(session);
  openai_provider_destory(provider);
  agent_destroy(agent);

  return 0;
}