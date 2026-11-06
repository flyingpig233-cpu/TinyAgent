#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "agent/providers/openai.h"

int main(void) {
  Agent *agent = NULL;
  Session *session = NULL;
  LLMProvider *provider = NULL;
  String output = {0};
  AgentError error = {0};
  AgentConfig config = config_default();
  const char *api_key = getenv("DEEPSEEK_API_KEY");
  if (api_key == NULL)
    return 0;
  OpenAIConfig provider_config = {
      .common = {.api_key = api_key,
                 .base_url = "https://api.deepseek.com/",
                 .model = "deepseek-flash"},
      .protocol = PROTOCOL_RESPONSES};
  int status = openai_provider_init(&provider, &provider_config);
  if (status != AGENT_OK)
    goto done;
  status =
      session_init(&session, "Test Session", "You are a helpful assistant");
  if (status != AGENT_OK)
    goto done;
  status = agent_create(&config, provider, &agent);
  if (status != AGENT_OK)
    goto done;

  char user_input[512];
  printf("init successfully\n");
  for (;;) {
    printf("Input: ");
    fflush(stdout);
    if (fgets(user_input, sizeof(user_input), stdin) == NULL)
      break;
    user_input[strcspn(user_input, "\r\n")] = '\0';
    if (strcmp(user_input, "/exit") == 0)
      break;
    int result = agent_run(agent, session, user_input, &output, &error);
    if (result != AGENT_OK) {
      fprintf(stderr, "failed to get response from LLM: %s (error code: %d)\n",
              error.message, result);
      continue;
    }
    printf("Output: %s\n", string_c_str(&output));
  }

done:
  if (status != AGENT_OK)
    fprintf(stderr, "Initialization failed (error code: %d).\n", status);
  string_destroy(&output);
  agent_destroy(agent);
  session_destory(session);
  if (provider != NULL)
    openai_provider_destory(provider);
  return status == AGENT_OK ? 0 : 1;
}
