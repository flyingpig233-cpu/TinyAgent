# TinyAgent

A small C17 agent with a reusable HTTP transport and an OpenAI-compatible provider.

## Build and test

Requires CMake, a C17 compiler, libcurl, and libcJSON. Python 3 enables the local
HTTP integration tests. No API key is needed to run the tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## OpenAI protocols

One provider supports non-streaming Chat Completions and Responses:

```c
OpenAIConfig config = {
    .common = {
        .api_key = api_key,
        .base_url = "https://api.openai.com/v1",
        .model = model,
    },
    .protocol = PROTOCOL_RESPONSES, // Or PROTOCOL_CHAT_COMPLETIONS (default 0).
};
LLMProvider *provider = NULL;
int status = openai_provider_init(&provider, &config);
```

`common.base_url` is the **API root**, including any version or proxy prefix.
The provider appends `/responses` or `/chat/completions` and removes trailing
slashes before joining. Existing configurations that put the full endpoint in
`base_url` must move it to the optional `endpoint_url` override. Configuration
strings are copied during initialization. Release the provider with
`openai_provider_destory(provider)`.

The CLI in `app/main.c` reads `DEEPSEEK_API_KEY` and uses the DeepSeek API root,
the `deepseek-flash` model, and Responses. To use another endpoint or
protocol, explicitly change the `OpenAIConfig` in your application, as shown
above. Select a model and server that support the chosen protocol.

```sh
export DEEPSEEK_API_KEY='your-key'
./build/app/tiny-agent
```

### History and results

Responses requests use `store: false` and request encrypted reasoning content.
Each decoded response contains normalized text, custom function calls, and an
owned `continuation` containing the complete ordered output items. Use
`context_add_assistant_response()` before destroying the response: context takes
an independent copy. The next Responses request replays those items, preserving
reasoning state and function item IDs without duplicating normalized text or
calls. Each session owns its history; the provider keeps no response ID or
conversation state. The same HTTP client must not be used concurrently.

Both codecs normalize completion, tool calls, truncation, filtering, and refusal.
`provider_status`, `provider_reason`, and `refusal` retain diagnostic information;
failed responses and HTTP errors report server error details through `AgentError`.
All response fields are freed by `llm_response_destroy()`.

### Custom function tools

Use `LLMRequest.tools` and `tool_count` to supply borrowed `ToolDefinition`
entries. Each definition contains a name, optional description, a JSON Schema
object in `parameters_json`, and an explicit `strict` value (0 or 1). Strict
schemas must satisfy the API's strict-mode requirements. The provider encodes the
appropriate tool schema for each protocol.

For a tool round trip, append the decoded assistant response to context, execute
the calls, and append one `TOOL_MESSAGE` per result using the returned
`ToolCall.call_id` as `tool_call_id`. In Responses, the `fc_...` item ID is distinct
from the `call_id` used to associate results. Then call the provider again with
the updated context and tool definitions.

The provider supports these function round trips, but the current `agent_run()`
still reports `AGENT_ERR_TOOL_UNSUPPORTED` when a tool needs execution; an agent
tool registry/executor is a separate feature. Hosted tools, multimodal content,
streaming, and server-managed conversation IDs are not implemented. Unsupported
Responses output item types produce an explicit error.

Protocol references: [OpenAI migration guide](https://developers.openai.com/api/docs/guides/migrate-to-responses)
and [function calling](https://developers.openai.com/api/docs/guides/function-calling).
