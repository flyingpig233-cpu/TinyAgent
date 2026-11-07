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

The local fake-server drivers bypass inherited proxies for loopback hosts in
their own process environment; production requests still honor proxy settings.

## OpenAI protocols

One provider supports Chat Completions and Responses (streaming optional):

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

The CLI allows 180 seconds for each model request. Override it
with `DEEPSEEK_TIMEOUT_MS`, a positive integer in milliseconds:

```sh
DEEPSEEK_TIMEOUT_MS=300000 ./build/app/tiny-agent
```

### Tool approval

`write_file`, `edit_file` and `run_command` are marked mutating; `list_dir`,
`read_file` and `search` are read-only. In an interactive TTY session the CLI
prompts before each mutating call (`y` allow, `n` deny, `a` allow all for the
rest of the session). Read-only tools never prompt. A one-shot `-p/--print` run
or a piped stdin has no one to answer the prompt, so mutating tools are denied
unless you pass `--yes`; `--read-only` always denies them, and `--yes` combined
with `--read-only` is rejected as a usage error.

Hosts embedding the library set `AgentConfig.on_tool_approval` to receive every
call with the tool's `ToolDefinition.mutating` flag and return
`TOOL_DECISION_ALLOW`, `TOOL_DECISION_DENY`, or `TOOL_DECISION_ALLOW_ALL` (which
sticks for the rest of the session). A NULL callback allows everything. A
denied call becomes a tool error result, so the model can inspect it and adapt
rather than aborting the run.

### Machine-readable output

Pass `--json` (or `--output-format json`) with a prompt to get a single JSON
object on stdout instead of plain text:

```json
{"status":"ok","output":"There are 2 files.","error":null,"error_code":null,
 "rounds":1,"model":"deepseek-flash","protocol":"responses","duration_ms":812}
```

On failure `status` is `"error"`, `output` is `null`, and `error` / `error_code`
carry the message and the numeric `AgentStatus`. The per-round transcript, when
enabled with `-v`, still goes to stderr, so stdout parses cleanly. `--json`
requires a prompt (`-p` or positional).

Exit codes: `0` success, `1` runtime error, `2` usage or configuration error,
and `130` when a run is interrupted with Ctrl-C.

### Streaming output

Pass `--stream` to print assistant text as it is generated. Both protocols are
supported; the final response is still fully assembled, so tool calls, history
and continuation items behave exactly as in the non-streaming path. Deltas go to
stdout, while an optional `-v` transcript stays on stderr. `--json` takes
precedence: when both are set, output is a single JSON object and nothing is
streamed.

Library callers set `AgentConfig.stream = 1` and an `on_delta` callback
(`void (*)(void *userdata, int round, const char *text)`). Providers receive the
same request through `LLMRequest.stream` / `LLMRequest.on_delta`; a provider that
does not support streaming may ignore these fields and return a complete
response in one piece.

Malformed stream JSON and invalid Chat Completions tool-call indexes return
`AGENT_ERR_LLM_RESPONSE`. New tool-call indexes must be contiguous from zero;
later deltas may revisit any existing index.

`ProviderConfig.connect_timeout_ms` and `ProviderConfig.timeout_ms` configure
connection and total request deadlines for C callers. Zero preserves the HTTP
defaults of 10 seconds to connect and 60 seconds total; negative values are
rejected. A timeout with zero bytes received means no response body arrived
before the deadline; it can result from network/proxy issues, queueing or slow
generation. Increasing the timeout alone cannot diagnose the underlying cause.

Collected HTTP bodies use geometrically grown buffers rather than reallocating
for every received chunk. Binary payloads retain their exact `body_len` and a
separate trailing NUL terminator.

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
results, and continues until a final response or `max_round` is reached. Tool
errors are returned to the model so it can correct and retry the call.
`max_tool_retries` limits retries after consecutive failed tool rounds (default
3 when zero); a successful tool round resets the count. `max_round` still
limits all model requests. Hosted
tools, multimodal content, and server-managed conversation IDs are not implemented. Unsupported
Responses output item types produce an explicit error.

### Progress events

Set `AgentConfig.on_event` (and an optional `userdata`) to observe a run without
changing its behavior:

```c
static void on_event(void *userdata, const AgentEvent *event) {
  (void)userdata;
  switch (event->type) {
  case AGENT_EVENT_LLM_RESPONSE:
    if (event->content != NULL) printf("assistant: %s\n", event->content);
    break;
  case AGENT_EVENT_TOOL_CALL:
    printf("call %s(%s)\n", event->tool_name, event->arguments_json);
    break;
  case AGENT_EVENT_TOOL_RESULT:
    printf("result %s: %s\n", event->tool_name,
           event->tool_output ? event->tool_output : "(none)");
    break;
  default:
    break;
  }
}

AgentConfig config = config_default();
config.on_event = on_event;
```

Events fire synchronously in this order: `AGENT_EVENT_RUN_START`, then per round
`AGENT_EVENT_LLM_RESPONSE` and, when the model requests tools,
`AGENT_EVENT_TOOL_CALL` / `AGENT_EVENT_TOOL_RESULT`, and finally
`AGENT_EVENT_RUN_END`. `round` is 1-based for response and tool events. All
string fields are borrowed and only valid for the duration of the callback, so
copy anything that must outlive it. `AGENT_EVENT_RUN_END` carries the final
status, the output on success, and the error message on failure. The CLI in
`app/main.c` uses this callback to print a per-round transcript of model
responses and tool activity:

```text
Input: list the files

=== agent run ===

--- round 1 ---
[round 1] model: tool_calls (1 tool call)
[round 1] assistant: I'll check the directory.
[round 1] -> list_dir({"path":"."})
[round 1] <- list_dir ok
    {"entries":["a.c","b.h"]}

--- round 2 ---
[round 2] model: stop
[run] complete (2 rounds)
Output: There are two files.
```

### Cancellation

Runs are cooperatively cancellable. In the CLI, Ctrl-C during a request or a
tool call aborts the in-flight work and returns to the prompt (interactive) or
exits `130` (one-shot); Ctrl-C at the prompt just clears the line. Cancelled
`--json` runs still emit one JSON object, with `status: "error"` and
`error_code: 14` (`AGENT_ERR_CANCELLED`).

Hosts opt in through the same poll hook at three layers:

- `AgentConfig.is_cancelled` / `cancel_userdata` is polled before each round,
  before each tool call, and periodically while a provider request or tool
  subprocess is running. A nonzero return aborts with `AGENT_ERR_CANCELLED`.
- `LLMRequest.is_cancelled` / `cancel_userdata` reaches providers directly.
  The OpenAI provider maps it onto libcurl's transfer-info callback.
- `HttpRequest.on_progress` / `progress_userdata` is a raw libcurl hook: it is
  called roughly every 100 ms, and a nonzero return aborts the transfer. It
  can also be used to pump progress UI.

`ToolContext.is_cancelled` is passed to every tool callback, so long-running
`run_command` and `search` invocations kill their process group promptly
instead of waiting for their timeout.

When a tool round is cancelled, completed tool results are retained and every
remaining call receives an error result without being executed. This preserves
valid conversation history so the same session can accept the next prompt.

The hooks are only as fast as the polling: a blocked system call cannot be
interrupted mid-flight, but libcurl and the tool process loop both poll every
~100 ms or less.

### Terminal UI

The CLI renders through a single writer. Prompt and the live status line
(`thinking  1.2s (round 2)`) go to stderr, answers and streamed text go to
stdout, and the per-round transcript stays on its documented sink. The status
line uses ANSI cursor movement, so it is disabled when stderr is not a TTY or
`TERM=dumb` or `NO_COLOR` is set; `--json` never draws it. Piped and `--print`
runs therefore keep stdout byte-clean.

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

`list_dir` requires UTF-8 entry names. If any name contains invalid bytes, it
returns an argument error with no partial listing, rather than renaming the
entry or placing invalid UTF-8 in the next provider request.

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
