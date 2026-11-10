#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "infra/platform.h"

#include "agent/agent.h"
#include "agent/infra.h"
#include "agent/providers/openai.h"
#include "agent/tools/file_system/edit_file.h"
#include "agent/tools/file_system/list_dir.h"
#include "agent/tools/file_system/read_file.h"
#include "agent/tools/file_system/search.h"
#include "agent/tools/file_system/write_file.h"
#include "agent/tools/run_command.h"
#include "ui.h"

#define CLI_DEFAULT_BASE_URL "https://api.deepseek.com/"
#define CLI_DEFAULT_MODEL "deepseek-flash"
#define CLI_DEFAULT_SYSTEM_PROMPT "You are a helpful assistant"
#define CLI_DEFAULT_SESSION_ID "cli"
#define CLI_DEFAULT_TIMEOUT_MS 180000L
#define CLI_MAX_TIMEOUT_MS 3600000L
#define CLI_MAX_ROUNDS 100000L

/* Process exit codes. 130 is the conventional 128 + SIGINT. */
enum {
  CLI_EXIT_OK = 0,
  CLI_EXIT_RUNTIME = 1,
  CLI_EXIT_USAGE = 2,
  CLI_EXIT_INTERRUPTED = 130
};

/* ------------------------------------------------------------------ */
/* Interrupt handling                                                  */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_interrupt = 0;
/* Set while agent_run() is executing. Used only to decide whether echoing
 * "^C" is safe without trampling the live status line. */
static volatile sig_atomic_t g_run_active = 0;
/* Set once the full-screen backend owns the terminal. The signal handler only
 * sets flags; ncurses is never touched from a handler. */
static volatile sig_atomic_t g_fullscreen_active = 0;
/* Signal that requested termination in full-screen mode, or 0. */
static volatile sig_atomic_t g_terminate_signal = 0;

static void on_sigint(int sig) {
  (void)sig;
  /* Setting the shared flag is async-signal-safe. The UI polls it and aborts
   * the in-flight request or tool; libcurl sees it through CURLOPT_XFERINFO. */
  g_interrupt = 1;
  if (!g_run_active && !g_fullscreen_active) {
#if defined(_WIN32)
    fputs("^C\n", stderr);
    fflush(stderr);
#else
    static const char message[] = "^C\n";
    ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
#endif
  }
}

static void on_terminate(int sig) {
  g_terminate_signal = sig;
  g_interrupt = 1;
}

#if defined(_WIN32)

static BOOL WINAPI on_console_ctrl(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    on_sigint(0);
    return TRUE;
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    on_terminate(SIGTERM);
    return TRUE;
  default:
    return FALSE;
  }
}

static void install_signal_handlers(void) {
  (void)SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

/* The console handler covers termination; there is nothing to swap around the
 * full-screen backend (which is not built on Windows). */
static void install_terminate_handlers(void) {}
static void restore_terminate_handlers(void) {}

#else

static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  /* No SA_RESTART: blocking reads should return EINTR so the REPL can react. */
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);

  struct sigaction ignore;
  memset(&ignore, 0, sizeof(ignore));
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  ignore.sa_flags = 0;
  sigaction(SIGPIPE, &ignore, NULL);
}

/* Installed around the full-screen backend. If startup fails, the defaults are
 * restored so line mode keeps its normal signal behavior. */
static void install_terminate_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_terminate;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
}

static void restore_terminate_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
}

#endif

/* ------------------------------------------------------------------ */
/* Command-line options                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *api_key;
  const char *base_url;
  const char *model;
  const char *system_prompt;
  const char *session_id;
  const char *prompt; /* NULL means interactive. */
  OpenAIProtocol protocol;
  long timeout_ms;
  long connect_timeout_ms;
  int max_round;
  int quiet;
  int verbose;
  int output_json; /* --json / --output-format json: machine-readable result. */
  int stream;      /* --stream: print assistant text as it is generated. */
  int yes;         /* --yes: auto-approve mutating tools. */
  int read_only;   /* --read-only: deny mutating tools. */
  int no_tui;      /* --no-tui: keep the line-oriented interactive REPL. */
} CliOptions;

typedef enum {
  ARGS_OK = 0,
  ARGS_EXIT, /* --help / --version: print done, exit success. */
  ARGS_ERROR
} ArgsResult;

static void print_help(const char *program) {
  printf(
      "Usage: %s [options] [prompt]\n"
      "\n"
      "Run the TinyAgent coding agent. With no prompt it starts an "
      "interactive\n"
      "session; with -p/--print (or a positional prompt) it runs once and "
      "exits.\n"
      "\n"
      "While a request or tool is running, Ctrl-C cancels it and returns to "
      "the\n"
      "prompt (interactive) or exits 130 (one-shot).\n"
      "\n"
      "Mutating tools (write_file, edit_file, run_command) ask for approval "
      "in\n"
      "interactive TTY sessions. Non-interactive runs deny them unless --yes "
      "is\n"
      "set; --read-only always denies them. Read-only tools never prompt.\n"
      "\n"
      "Options:\n"
      "  -h, --help                 Show this help and exit\n"
      "  -V, --version              Print version and exit\n"
      "  -p, --print <prompt>       Run one prompt non-interactively; the "
      "answer\n"
      "                             goes to stdout, diagnostics to stderr\n"
      "  -m, --model <name>         Model name (default: %s)\n"
      "      --base-url <url>       API root URL (default: %s)\n"
      "      --api-key <key>        API key (default: $TINY_AGENT_API_KEY or\n"
      "                             $DEEPSEEK_API_KEY)\n"
      "      --protocol <name>      chat | responses (default: responses)\n"
      "      --timeout <ms>         Per-request total timeout (default: %ld)\n"
      "      --connect-timeout <ms> Connect timeout, 0 = library default\n"
      "      --max-rounds <n>       Max model/tool rounds per run (default: "
      "%d)\n"
      "      --system-prompt <text> System prompt (default: \"%s\")\n"
      "      --session-id <id>      Session identifier (default: %s)\n"
      "  -y, --yes                  Auto-approve mutating tools\n"
      "      --read-only            Deny mutating tools; read-only tools run\n"
      "      --no-tui               Keep the line-oriented interactive REPL\n"
      "  -q, --quiet                Suppress the per-round transcript\n"
      "  -v, --verbose              Print the transcript to stderr in --print "
      "mode\n"
      "  -o, --output-format <fmt>  text | json (default: text)\n"
      "      --json                 Shorthand for --output-format json; emits\n"
      "                             one JSON object on stdout (needs a "
      "prompt)\n"
      "      --stream               Stream assistant text as it is generated\n"
      "\n"
      "Environment:\n"
      "  TINY_AGENT_API_KEY / DEEPSEEK_API_KEY, DEEPSEEK_TIMEOUT_MS, NO_COLOR\n"
      "\n"
      "Exit codes: 0 success, 1 runtime error, 2 usage error, 130 "
      "interrupted\n",
      program, CLI_DEFAULT_MODEL, CLI_DEFAULT_BASE_URL, CLI_DEFAULT_TIMEOUT_MS,
      CONFIG_DEFAULT_MAX_ROUND, CLI_DEFAULT_SYSTEM_PROMPT,
      CLI_DEFAULT_SESSION_ID);
}

/* Accepts "--name value" and "--name=value". Returns 1 when matched, 0 when
 * not, -1 when matched but the value is missing. */
static int take_value(int argc, char **argv, int *i, const char *name,
                      const char **value) {
  const char *arg = argv[*i];
  size_t n = strlen(name);
  if (strncmp(arg, name, n) != 0)
    return 0;
  if (arg[n] == '=')
    return *value = arg + n + 1, 1;
  if (arg[n] != '\0')
    return 0;
  if (*i + 1 >= argc)
    return -1;
  *value = argv[++(*i)];
  return 1;
}

static int parse_long_value(const char *text, long *out, long min, long max) {
  if (text == NULL || *text == '\0')
    return 0;
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || value < min ||
      value > max)
    return 0;
  *out = value;
  return 1;
}

static int parse_protocol(const char *text, OpenAIProtocol *out) {
  if (strcmp(text, "chat") == 0 || strcmp(text, "chat_completions") == 0) {
    *out = PROTOCOL_CHAT_COMPLETIONS;
    return 1;
  }
  if (strcmp(text, "responses") == 0 || strcmp(text, "response") == 0) {
    *out = PROTOCOL_RESPONSES;
    return 1;
  }
  return 0;
}

static ArgsResult parse_args(int argc, char **argv, CliOptions *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->protocol = PROTOCOL_RESPONSES;
  opts->timeout_ms = -1; /* sentinel: not set by flag yet. */
  opts->connect_timeout_ms = 0;
  opts->max_round = CONFIG_DEFAULT_MAX_ROUND;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = NULL;
    int match = 0;

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_help(argv[0]);
      return ARGS_EXIT;
    }
    if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
      printf("tiny-agent %s\n", agent_version());
      return ARGS_EXIT;
    }
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
      opts->quiet = 1;
      continue;
    }
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
      opts->verbose = 1;
      continue;
    }
    if (strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0) {
      opts->yes = 1;
      continue;
    }
    if (strcmp(arg, "--read-only") == 0) {
      opts->read_only = 1;
      continue;
    }
    if (strcmp(arg, "--no-tui") == 0) {
      opts->no_tui = 1;
      continue;
    }
    if (strcmp(arg, "--json") == 0) {
      opts->output_json = 1;
      continue;
    }
    if (strcmp(arg, "--stream") == 0) {
      opts->stream = 1;
      continue;
    }

    if ((match = take_value(argc, argv, &i, "-p", &value)) ||
        (match = take_value(argc, argv, &i, "--print", &value))) {
      if (match < 0)
        goto missing_value;
      opts->prompt = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "-m", &value)) ||
        (match = take_value(argc, argv, &i, "--model", &value))) {
      if (match < 0)
        goto missing_value;
      opts->model = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--base-url", &value))) {
      if (match < 0)
        goto missing_value;
      opts->base_url = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--api-key", &value))) {
      if (match < 0)
        goto missing_value;
      opts->api_key = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--protocol", &value))) {
      if (match < 0)
        goto missing_value;
      if (!parse_protocol(value, &opts->protocol)) {
        fprintf(stderr,
                "error: invalid --protocol '%s' (use chat or responses)\n",
                value);
        return ARGS_ERROR;
      }
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--timeout", &value))) {
      if (match < 0)
        goto missing_value;
      if (!parse_long_value(value, &opts->timeout_ms, 1, CLI_MAX_TIMEOUT_MS)) {
        fprintf(stderr, "error: --timeout must be an integer in [1, %ld] ms\n",
                CLI_MAX_TIMEOUT_MS);
        return ARGS_ERROR;
      }
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--connect-timeout", &value))) {
      if (match < 0)
        goto missing_value;
      if (!parse_long_value(value, &opts->connect_timeout_ms, 0,
                            CLI_MAX_TIMEOUT_MS)) {
        fprintf(stderr,
                "error: --connect-timeout must be an integer in [0, %ld] ms\n",
                CLI_MAX_TIMEOUT_MS);
        return ARGS_ERROR;
      }
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--max-rounds", &value))) {
      if (match < 0)
        goto missing_value;
      long rounds = 0;
      if (!parse_long_value(value, &rounds, 1, CLI_MAX_ROUNDS)) {
        fprintf(stderr, "error: --max-rounds must be an integer in [1, %ld]\n",
                CLI_MAX_ROUNDS);
        return ARGS_ERROR;
      }
      opts->max_round = (int)rounds;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--system-prompt", &value))) {
      if (match < 0)
        goto missing_value;
      opts->system_prompt = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "--session-id", &value))) {
      if (match < 0)
        goto missing_value;
      opts->session_id = value;
      continue;
    }
    if ((match = take_value(argc, argv, &i, "-o", &value)) ||
        (match = take_value(argc, argv, &i, "--output-format", &value))) {
      if (match < 0)
        goto missing_value;
      if (strcmp(value, "json") == 0) {
        opts->output_json = 1;
      } else if (strcmp(value, "text") == 0) {
        opts->output_json = 0;
      } else {
        fprintf(stderr, "error: --output-format must be text or json\n");
        return ARGS_ERROR;
      }
      continue;
    }

    if (arg[0] == '-' && arg[1] != '\0') {
      fprintf(stderr, "error: unknown option '%s' (try --help)\n", arg);
      return ARGS_ERROR;
    }
    /* Positional argument: treat the first one as the prompt. */
    if (opts->prompt == NULL)
      opts->prompt = arg;
    else {
      fprintf(stderr, "error: unexpected argument '%s'\n", arg);
      return ARGS_ERROR;
    }
    continue;

  missing_value:
    fprintf(stderr, "error: option '%s' requires a value\n", arg);
    return ARGS_ERROR;
  }

  /* Resolve defaults and environment fallbacks (flags already win). */
  if (opts->yes && opts->read_only) {
    fprintf(stderr, "error: --yes and --read-only are mutually exclusive\n");
    return ARGS_ERROR;
  }
  if (opts->output_json && opts->prompt == NULL) {
    fprintf(stderr, "error: --json requires a prompt (-p or positional)\n");
    return ARGS_ERROR;
  }
  if (opts->model == NULL)
    opts->model = CLI_DEFAULT_MODEL;
  if (opts->base_url == NULL)
    opts->base_url = CLI_DEFAULT_BASE_URL;
  if (opts->system_prompt == NULL)
    opts->system_prompt = CLI_DEFAULT_SYSTEM_PROMPT;
  if (opts->session_id == NULL)
    opts->session_id = CLI_DEFAULT_SESSION_ID;

  if (opts->api_key == NULL) {
    const char *env = getenv("TINY_AGENT_API_KEY");
    if (env == NULL || env[0] == '\0')
      env = getenv("DEEPSEEK_API_KEY");
    opts->api_key = env;
  }

  if (opts->timeout_ms < 0) {
    long timeout = CLI_DEFAULT_TIMEOUT_MS;
    const char *env = getenv("DEEPSEEK_TIMEOUT_MS");
    if (env != NULL && env[0] != '\0') {
      if (!parse_long_value(env, &timeout, 1, CLI_MAX_TIMEOUT_MS)) {
        fprintf(
            stderr,
            "error: DEEPSEEK_TIMEOUT_MS must be an integer in [1, %ld] ms\n",
            CLI_MAX_TIMEOUT_MS);
        return ARGS_ERROR;
      }
    }
    opts->timeout_ms = timeout;
  }

  return ARGS_OK;
}

/* ------------------------------------------------------------------ */
/* Reading input without a fixed-length buffer                         */
/* ------------------------------------------------------------------ */

typedef enum { READ_OK = 0, READ_EOF, READ_INTERRUPTED, READ_ERROR } ReadResult;

/* Reads one line (without the trailing newline) into a freshly allocated
 * buffer. The caller owns *out. Handles CRLF and arbitrary line lengths. */
static ReadResult read_line(FILE *in, char **out) {
  size_t capacity = 128;
  size_t length = 0;
  char *buffer = malloc(capacity);
  if (buffer == NULL)
    return READ_ERROR;

  for (;;) {
    errno = 0;
    int ch = fgetc(in);
    if (ch == EOF) {
      if (ferror(in)) {
        clearerr(in);
        free(buffer);
        return errno == EINTR ? READ_INTERRUPTED : READ_ERROR;
      }
      if (length == 0) {
        free(buffer);
        return READ_EOF;
      }
      break; /* Last line without a trailing newline. */
    }
    if (ch == '\n')
      break;
    if (ch == '\r') {
      int next = fgetc(in);
      if (next != '\n' && next != EOF)
        ungetc(next, in);
      break;
    }
    if (length + 1 >= capacity) {
      size_t grown = capacity * 2;
      char *resized = realloc(buffer, grown);
      if (resized == NULL) {
        free(buffer);
        return READ_ERROR;
      }
      buffer = resized;
      capacity = grown;
    }
    buffer[length++] = (char)ch;
  }

  buffer[length] = '\0';
  *out = buffer;
  return READ_OK;
}

/* ------------------------------------------------------------------ */
/* Tool approval                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
  int auto_approve; /* --yes: approve every mutating tool. */
  int read_only;    /* --read-only: deny every mutating tool. */
  int interactive;  /* Prompt on a TTY; otherwise deny mutating tools. */
  Ui *ui;           /* Clears the live status line before prompting. */
} CliApproval;

/* Copies at most cap-1 bytes of the JSON arguments into buffer, adding an
 * ellipsis when truncated, so approval prompts stay readable. */
static void preview_arguments(const char *json, char *buffer, size_t cap) {
  if (json == NULL)
    json = "";
  size_t length = strlen(json);
  if (length < cap) {
    memcpy(buffer, json, length + 1);
    return;
  }
  if (cap <= 4) {
    if (cap > 0)
      buffer[0] = '\0';
    return;
  }
  memcpy(buffer, json, cap - 4);
  memcpy(buffer + (cap - 4), "...", 4); /* Includes the terminating NUL. */
}

static ToolDecision cli_approve(void *userdata, const char *tool_name,
                                const char *arguments_json, int mutating) {
  CliApproval *policy = userdata;
  if (!mutating)
    return TOOL_DECISION_ALLOW;
  if (policy == NULL || policy->read_only)
    return TOOL_DECISION_DENY;
  if (policy->auto_approve)
    return TOOL_DECISION_ALLOW;
  if (!policy->interactive)
    return TOOL_DECISION_DENY; /* Safe default when nobody can answer. */

  if (ui_is_fullscreen(policy->ui)) {
    ToolDecision decision = ui_approve(policy->ui, tool_name, arguments_json);
    ui_set_phase(policy->ui, "thinking");
    return decision;
  }

  ui_clear_status(policy->ui);
  char preview[201];
  preview_arguments(arguments_json, preview, sizeof(preview));
  fprintf(stderr,
          "\n[tool] %s(%s)\n"
          "Allow this action? [y]es / [n]o / [a]ll: ",
          tool_name != NULL ? tool_name : "?", preview);
  fflush(stderr);

  char *line = NULL;
  if (read_line(stdin, &line) != READ_OK) {
    free(line);
    return TOOL_DECISION_DENY;
  }
  ToolDecision decision = TOOL_DECISION_DENY;
  if (line[0] == 'y' || line[0] == 'Y')
    decision = TOOL_DECISION_ALLOW;
  else if (line[0] == 'a' || line[0] == 'A')
    decision = TOOL_DECISION_ALLOW_ALL;
  free(line);
  ui_set_phase(policy->ui, "thinking");
  return decision;
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

static int report_run_error(int status, const AgentError *error,
                            long timeout_ms) {
  fprintf(stderr, "Agent run failed: %s (error code: %d)\n",
          error->message[0] ? error->message : "Unknown error", status);
  if (status == AGENT_ERR_HTTP_TIMEOUT)
    fprintf(stderr,
            "HTTP request timed out (total limit: %ld ms). Check network/proxy "
            "and API availability, or increase --timeout.\n",
            timeout_ms);
  return EXIT_FAILURE;
}

static long monotonic_ms(void) { return (long)ta_monotonic_ms(); }

/* Emits a JSON string literal, escaping quotes, backslashes and control
 * characters. Valid UTF-8 bytes pass through unchanged. */
static void json_write_string(FILE *out, const char *text) {
  if (text == NULL)
    text = "";
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
    switch (*p) {
    case '"':
      fputs("\\\"", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '\b':
      fputs("\\b", out);
      break;
    case '\f':
      fputs("\\f", out);
      break;
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    default:
      if (*p < 0x20)
        fprintf(out, "\\u%04x", *p);
      else
        fputc(*p, out);
    }
  }
  fputc('"', out);
}

/* Prints one JSON result object on stdout. No human text goes to stdout in
 * this mode, so the output can be piped directly into a JSON parser. */
static int print_json_result(int status, const char *output,
                             const char *error_message, int rounds,
                             const CliOptions *opts, long duration_ms) {
  FILE *out = stdout;
  fputs("{\"status\":", out);
  json_write_string(out, status == AGENT_OK ? "ok" : "error");
  fputs(",\"output\":", out);
  if (status == AGENT_OK)
    json_write_string(out, output);
  else
    fputs("null", out);
  fputs(",\"error\":", out);
  if (status == AGENT_OK)
    fputs("null", out);
  else
    json_write_string(out, error_message);
  fputs(",\"error_code\":", out);
  if (status == AGENT_OK)
    fputs("null", out);
  else
    fprintf(out, "%d", status);
  fprintf(out, ",\"rounds\":%d", rounds);
  fputs(",\"model\":", out);
  json_write_string(out, opts->model);
  fputs(",\"protocol\":", out);
  json_write_string(out, opts->protocol == PROTOCOL_RESPONSES
                             ? "responses"
                             : "chat_completions");
  fprintf(out, ",\"duration_ms\":%ld}\n", duration_ms);
  return fflush(out) == EOF ? CLI_EXIT_RUNTIME : CLI_EXIT_OK;
}

static int run_once(Agent *agent, Session *session, const CliOptions *opts,
                    Ui *ui) {
  String output = {0};
  AgentError error = {0};
  int exit_code = CLI_EXIT_OK;

  ui_reset_run(ui);
  g_interrupt = 0;
  long started = monotonic_ms();
  g_run_active = 1;
  int status = agent_run(agent, session, opts->prompt, &output, &error);
  g_run_active = 0;
  long duration_ms = monotonic_ms() - started;
  int cancelled = status == AGENT_ERR_CANCELLED;
  g_interrupt = 0;

  if (opts->output_json) {
    const char *message = error.message[0] ? error.message : "Unknown error";
    exit_code = print_json_result(
        status, status == AGENT_OK ? string_c_str(&output) : NULL, message,
        ui_rounds(ui), opts, duration_ms);
    if (status != AGENT_OK && exit_code == CLI_EXIT_OK)
      exit_code = CLI_EXIT_RUNTIME;
    if (cancelled)
      exit_code = CLI_EXIT_INTERRUPTED;
  } else if (cancelled) {
    fprintf(stderr, "Cancelled.\n");
    exit_code = CLI_EXIT_INTERRUPTED;
  } else if (status != AGENT_OK) {
    exit_code = report_run_error(status, &error, opts->timeout_ms);
  } else if (opts->stream && ui_streamed(ui)) {
    if (ui_finish_stream(ui) != 0) {
      fprintf(stderr, "Failed to write output.\n");
      exit_code = CLI_EXIT_RUNTIME;
    }
  } else if (ui_print_answer(string_c_str(&output)) != 0) {
    fprintf(stderr, "Failed to write output.\n");
    exit_code = CLI_EXIT_RUNTIME;
  }

  string_destroy(&output);
  return exit_code;
}

static void ui_report_run_error(Ui *ui, int status, const AgentError *error,
                                long timeout_ms) {
  char message[512];
  snprintf(message, sizeof(message),
           "Agent run failed: %.300s (error code: %d)",
           error->message[0] ? error->message : "Unknown error", status);
  ui_message(ui, message);
  if (status == AGENT_ERR_HTTP_TIMEOUT) {
    char advice[256];
    snprintf(advice, sizeof(advice),
             "HTTP request timed out (total limit: %ld ms). Check "
             "network/proxy and API availability, or increase --timeout.",
             timeout_ms);
    ui_message(ui, advice);
  }
}

static int run_interactive(Agent *agent, Session *session,
                           const CliOptions *opts, int show_prompt, Ui *ui) {
  int exit_code = EXIT_SUCCESS;
  int fullscreen = ui_is_fullscreen(ui);
  if (show_prompt && !fullscreen) {
    fprintf(stderr,
            "TinyAgent %s  model=%s  protocol=%s\n"
            "Type /help for commands, /exit to quit. Ctrl-C cancels a run.\n",
            agent_version(), opts->model,
            opts->protocol == PROTOCOL_RESPONSES ? "responses"
                                                 : "chat_completions");
  }

  for (;;) {
    if (g_terminate_signal != 0) {
      exit_code = 128 + (int)g_terminate_signal;
      break;
    }
    g_interrupt = 0;

    char *line = NULL;
    if (fullscreen) {
      ui_prompt(ui);
      UiInputResult input = ui_read_input(ui, &line);
      if (g_terminate_signal != 0) {
        free(line);
        exit_code = 128 + (int)g_terminate_signal;
        break;
      }
      if (input == UI_INPUT_EOF)
        break;
      if (input == UI_INPUT_ERROR) {
        exit_code = EXIT_FAILURE;
        break;
      }
      if (input == UI_INPUT_INTERRUPTED) {
        free(line);
        g_interrupt = 0;
        continue;
      }
    } else {
      ui_prompt(ui);
      ReadResult read = read_line(stdin, &line);
      if (read == READ_INTERRUPTED) {
        fputc('\n', stderr);
        g_interrupt = 0; /* First Ctrl-C at the prompt clears the line. */
        continue;
      }
      if (read == READ_EOF)
        break;
      if (read == READ_ERROR) {
        fprintf(stderr, "Failed to read input.\n");
        exit_code = EXIT_FAILURE;
        break;
      }
    }

    if (line == NULL)
      continue;

    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
      free(line);
      break;
    }
    if (strcmp(line, "/help") == 0) {
      if (fullscreen) {
        ui_message(ui, "Commands:\n"
                       "  /help   Show this help\n"
                       "  /exit   Quit (also /quit or Ctrl-D)\n"
                       "Keys: Enter send, Ctrl-J newline, Up/Down history,\n"
                       "      PgUp/PgDn scroll, Ctrl-End bottom, Ctrl-C "
                       "cancel,\n"
                       "      Ctrl-D exit when the input is empty.");
      } else {
        printf("Commands:\n"
               "  /help   Show this help\n"
               "  /exit   Quit (also /quit or Ctrl-D)\n");
      }
      free(line);
      continue;
    }
    if (line[0] == '\0') { /* Skip blank lines. */
      free(line);
      continue;
    }

    String output = {0};
    AgentError error = {0};
    ui_reset_run(ui);
    g_run_active = 1;
    int status = agent_run(agent, session, line, &output, &error);
    g_run_active = 0;
    free(line);
    if (g_terminate_signal != 0) {
      string_destroy(&output);
      exit_code = 128 + (int)g_terminate_signal;
      break;
    }
    if (status == AGENT_ERR_CANCELLED) {
      if (!fullscreen)
        fprintf(stderr, "\n[cancelled]\n");
      g_interrupt = 0;
      string_destroy(&output);
      if (ui_failed(ui)) {
        exit_code = EXIT_FAILURE;
        break;
      }
      continue;
    }
    if (status != AGENT_OK) {
      if (fullscreen) {
        if (!ui_run_ended(ui))
          ui_report_run_error(ui, status, &error, opts->timeout_ms);
      } else {
        exit_code = report_run_error(status, &error, opts->timeout_ms);
      }
      string_destroy(&output);
      if (ui_failed(ui)) {
        exit_code = EXIT_FAILURE;
        break;
      }
      continue;
    }
    if (!fullscreen) {
      if (opts->stream && ui_streamed(ui)) {
        if (ui_finish_stream(ui) != 0) {
          fprintf(stderr, "Failed to write output.\n");
          exit_code = CLI_EXIT_RUNTIME;
          string_destroy(&output);
          break;
        }
      } else if (ui_print_answer(string_c_str(&output)) != 0) {
        fprintf(stderr, "Failed to write output.\n");
        exit_code = CLI_EXIT_RUNTIME;
        string_destroy(&output);
        break;
      }
    }
    string_destroy(&output);
    if (ui_failed(ui)) {
      exit_code = EXIT_FAILURE;
      break;
    }
  }
  return exit_code;
}

/* The status line uses ANSI cursor movement, so disable it when the terminal
 * is dumb or the user opted out with NO_COLOR. */
static int terminal_supports_status(void) {
#if defined(_WIN32)
  return 0;
#else
  const char *term = getenv("TERM");
  if (term == NULL || strcmp(term, "dumb") == 0)
    return 0;
  const char *no_color = getenv("NO_COLOR");
  return no_color == NULL || no_color[0] == '\0';
#endif
}

/* The full-screen UI needs all three standard streams on a real terminal and a
 * usable terminfo entry. NO_COLOR only disables the line-mode status escape. */
static int terminal_can_fullscreen(void) {
#if defined(_WIN32)
  return 0;
#else
  const char *term = getenv("TERM");
  if (term == NULL || term[0] == '\0' || strcmp(term, "dumb") == 0)
    return 0;
  return ta_isatty(0) && ta_isatty(1) && ta_isatty(2);
#endif
}

int main(int argc, char **argv) {
  CliOptions opts;
  ArgsResult args = parse_args(argc, argv, &opts);
  if (args == ARGS_EXIT)
    return CLI_EXIT_OK;
  if (args == ARGS_ERROR)
    return CLI_EXIT_USAGE;

  if (opts.api_key == NULL || opts.api_key[0] == '\0') {
    fprintf(stderr,
            "error: no API key. Set TINY_AGENT_API_KEY or DEEPSEEK_API_KEY, or "
            "pass --api-key.\n");
    return CLI_EXIT_USAGE;
  }

  install_signal_handlers();

  int prompt_enabled = ta_isatty(0);

  /* Decide where the per-round transcript goes. stdout must stay clean when
   * the answer is meant to be consumed by another program. */
  FILE *transcript = NULL;
  if (opts.prompt != NULL) {
    transcript = opts.verbose ? stderr : NULL;
  } else if (prompt_enabled) {
    transcript = (opts.quiet || opts.stream) ? NULL : stdout;
  } else {
    transcript = opts.verbose ? stderr : NULL;
  }

  /* The live status line lives on stderr, never stdout, and only when stderr
   * is a real terminal. The prompt may still use stderr when ANSI escapes are
   * unavailable (for example the Windows console). --json keeps stderr for
   * diagnostics only. */
  int stderr_tty = ta_isatty(2);
  int status_tty =
      !opts.output_json && stderr_tty && terminal_supports_status();
  UiConfig ui_config = {.transcript = transcript,
                        .status =
                            (!opts.output_json && stderr_tty) ? stderr : NULL,
                        .tty = status_tty,
                        .stream_to_stdout = opts.stream && !opts.output_json,
                        .quiet = opts.quiet,
                        .cancel_flag = &g_interrupt,
                        .terminate_flag = &g_terminate_signal};
  Ui *ui = ui_create(&ui_config);
  if (ui == NULL) {
    fprintf(stderr, "error: could not initialize the terminal UI\n");
    return CLI_EXIT_RUNTIME;
  }

  /* Approval only prompts during an interactive TTY session; one-shot and
   * piped runs deny mutating tools unless --yes was passed. */
  CliApproval approval = {.auto_approve = opts.yes,
                          .read_only = opts.read_only,
                          .interactive = opts.prompt == NULL && prompt_enabled,
                          .ui = ui};

  Agent *agent = NULL;
  ToolRegistry *registry = NULL;
  Session *session = NULL;
  LLMProvider *provider = NULL;
  AgentError error = {0};
  AgentConfig config = config_default();
  config.max_round = opts.max_round;
  config.on_event = ui_on_event;
  config.userdata = ui;
  config.on_tool_approval = cli_approve;
  config.approval_userdata = &approval;
  config.stream = opts.stream && !opts.output_json;
  config.on_delta = ui_on_delta;
  config.is_cancelled = ui_poll;
  config.cancel_userdata = ui;
  int status = AGENT_OK;
  const char *stage = "provider initialization";
  int exit_code = EXIT_SUCCESS;

  OpenAIConfig provider_config = {
      .common = {.api_key = opts.api_key,
                 .base_url = opts.base_url,
                 .model = opts.model,
                 .connect_timeout_ms = opts.connect_timeout_ms,
                 .timeout_ms = opts.timeout_ms},
      .protocol = opts.protocol};
  status = openai_provider_init(&provider, &provider_config);
  if (status != AGENT_OK)
    goto done;

  stage = "tool registry initialization";
  status = tool_registry_init(&registry);
  if (status != AGENT_OK)
    goto done;
  stage = "tool registration";
  if ((status = list_dir_register(registry, &error)) != AGENT_OK ||
      (status = read_file_register(registry, &error)) != AGENT_OK ||
      (status = search_register(registry, &error)) != AGENT_OK ||
      (status = write_file_register(registry, &error)) != AGENT_OK ||
      (status = edit_file_register(registry, &error)) != AGENT_OK ||
      (status = run_command_register(registry, &error)) != AGENT_OK)
    goto done;

  stage = "session initialization";
  status = session_init(&session, opts.session_id, opts.system_prompt);
  if (status != AGENT_OK)
    goto done;
  stage = "agent initialization";
  status = agent_create(&config, provider, registry, &agent);
  if (status != AGENT_OK)
    goto done;

  int fullscreen = opts.prompt == NULL && !opts.no_tui && !opts.output_json &&
                   terminal_can_fullscreen();
  if (fullscreen) {
    const char *protocol =
        opts.protocol == PROTOCOL_RESPONSES ? "responses" : "chat_completions";
    install_terminate_handlers();
    if (ui_start_fullscreen(ui, opts.model, protocol, opts.session_id) != 0) {
      restore_terminate_handlers();
      fullscreen = 0;
      fprintf(stderr, "warning: full-screen UI unavailable; using line mode\n");
    } else {
      g_fullscreen_active = 1;
      install_signal_handlers();
    }
  }

  if (opts.prompt != NULL) {
    exit_code = run_once(agent, session, &opts, ui);
  } else {
    exit_code = run_interactive(agent, session, &opts,
                                prompt_enabled && !fullscreen, ui);
  }

done:
  g_fullscreen_active = 0;
  ui_stop_fullscreen(ui);
  if (ui_failed(ui)) {
    fprintf(stderr, "error: terminal UI failed\n");
    exit_code = EXIT_FAILURE;
  }
  if (status != AGENT_OK) {
    fprintf(stderr, "%s failed (error code: %d)%s%s.\n", stage, status,
            error.message[0] ? ": " : "", error.message);
    exit_code = EXIT_FAILURE;
  }
  agent_destroy(agent);
  if (session_destory(session) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy session.\n");
    exit_code = EXIT_FAILURE;
  }
  if (tool_registry_destory(registry) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy tool registry.\n");
    exit_code = EXIT_FAILURE;
  }
  if (provider != NULL && openai_provider_destory(provider) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy provider.\n");
    exit_code = EXIT_FAILURE;
  }
  ui_destroy(ui);
  return exit_code;
}
