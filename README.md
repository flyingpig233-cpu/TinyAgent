# TinyAgent

A small C17 agent with a reusable HTTP transport and an OpenAI-compatible provider.

## Build and test

Requires CMake, a C17 compiler, libcurl, and libcJSON. The coding tools target
POSIX systems; `search` also requires `rg` (ripgrep) on PATH. Python 3 enables the local
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

Use `LLMRequest.tools` and `tool_count` to supply a borrowed array of pointers
to `ToolDefinition` entries. Each definition contains a name, optional description, a JSON Schema
object in `parameters_json`, and an explicit `strict` value (0 or 1). Strict
schemas must satisfy the API's strict-mode requirements. The provider encodes the
appropriate tool schema for each protocol.

For a tool round trip, append the decoded assistant response to context, execute
the calls, and append one `TOOL_MESSAGE` per result using the returned
`ToolCall.call_id` as `tool_call_id`. In Responses, the `fc_...` item ID is distinct
from the `call_id` used to associate results. Then call the provider again with
the updated context and tool definitions.

`agent_run()` advertises registered tools, executes calls in order, appends their
results, and continues until a final response or `max_round` is reached. Hosted
tools, multimodal content, streaming, and server-managed conversation IDs are not implemented. Unsupported
Responses output item types produce an explicit error.

### Built-in coding tools

The CLI registers the following tools. Relative paths use the application's
current directory. Each public tool header defines its description, JSON schema,
borrowed `ToolDefinition`, callback and registration helper.

| Tool | Arguments | Result |
| --- | --- | --- |
| `list_dir` | `path` | Immediate entries with name, type, size, mode and modification time |
| `read_file` | `path` | Complete UTF-8 text, up to 1 MiB |
| `search` | `pattern`, nullable `path`, `glob`, `fixed_string`, `max_results` | JSON matching paths, 1-based line numbers, text, stderr, exit code and truncation/timeout flags |
| `write_file` | `path`, `content`, `mode`: `create` or `overwrite` | JSON path, mode and bytes written |
| `edit_file` | `path`, `old_text`, `new_text` | JSON path, replacement count and bytes written |
| `run_command` | `command`, nullable `cwd`, `timeout_ms`, `max_output_bytes` | JSON stdout, stderr, exit code, signal and truncation/timeout flags |

`search` runs ripgrep directly without shell interpolation. It supports regex
and literal matching, honors ignore files, and skips hidden files and symlinks
during directory traversal. Defaults are current directory, all file types,
regex matching and 100 matching lines; the result limit can be 1–1000. Execution
is limited to 10 seconds and 1 MiB of captured output. `truncated` also indicates
omitted lines or non-UTF-8 matches that cannot be represented as text. No matches
is a normal empty result; invalid regexes and missing executables set `is_error`.

`write_file` and `edit_file` accept UTF-8 text up to 1 MiB and require an existing
parent directory. They stage writes in the same directory and commit atomically,
reject symlinks/non-regular destinations, and keep existing permission bits
(excluding special bits). New files have mode 0600. `create` refuses any existing
path, while `overwrite` requires an existing file. Edits require a nonempty,
unique exact match, including overlapping occurrences; an empty `new_text`
deletes that match. Failed validation leaves the file unchanged. Modification
checks detect changes during editing but do not lock out concurrent writers.

`run_command` uses `/bin/sh -c`, closes stdin, and captures stdout and stderr
concurrently. Its defaults are the current directory, 30 seconds and 64 KiB of
combined captured output. Limits can be 1–300000 ms and 1–1048576 bytes. Excess
output is discarded while pipes continue to drain. Timeout kills the process
group and reaps the direct child. Invalid UTF-8 and NUL bytes become U+FFFD in
the returned text. The byte cap applies before this conversion and JSON escaping.
Commands inherit the application's environment and operating-system permissions.

Nullable arguments may be omitted by direct C callers; strict model schemas
require those keys with `null` to select defaults. Tool outputs are owned by the
caller and freed with `free(output->content); free(output);`. Command failures,
timeouts and ripgrep errors return `AGENT_OK` with `output->is_error = 1`, so the
model can inspect the result and continue. Invalid tool arguments, filesystem
errors and internal failures return a non-OK status and populate `AgentError`.

Protocol references: [OpenAI migration guide](https://developers.openai.com/api/docs/guides/migrate-to-responses)
and [function calling](https://developers.openai.com/api/docs/guides/function-calling).
