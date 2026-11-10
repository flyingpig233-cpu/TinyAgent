#ifndef TINY_AGENT_CLI_UI_H
#define TINY_AGENT_CLI_UI_H

#include <signal.h>
#include <stdio.h>

#include "agent/agent.h"

/* Single-writer terminal UI for the CLI.
 *
 * Every asynchronous source (agent events, streamed deltas, the cancel flag)
 * funnels through one Ui instance, so redraws never interleave. When the
 * terminal is not capable the Ui degrades to plain line output and never emits
 * ANSI escapes or touches stdout for status messages. */

#define UI_TOOL_OUTPUT_LIMIT 500

typedef struct Ui Ui;

typedef struct {
  FILE *transcript;     /* Per-round transcript sink; NULL disables. */
  FILE *status;         /* Spinner/state line sink; NULL disables. */
  int tty;              /* 1 enables the redrawn status line. */
  int stream_to_stdout; /* 1 when deltas go to stdout. */
  int quiet;            /* 1 hides tool/round details in the full-screen UI. */
  volatile sig_atomic_t *cancel_flag;    /* Optional shared cancel flag. */
  volatile sig_atomic_t *terminate_flag; /* Optional termination flag. */
} UiConfig;

typedef enum {
  UI_INPUT_OK = 0,
  UI_INPUT_EOF,
  UI_INPUT_INTERRUPTED,
  UI_INPUT_ERROR
} UiInputResult;

Ui *ui_create(const UiConfig *config);
void ui_destroy(Ui *ui);

/* Agent callbacks: wire these to AgentConfig.on_event / on_delta. */
void ui_on_event(void *userdata, const AgentEvent *event);
void ui_on_delta(void *userdata, int round, const char *text);

/* Cancel/tick hook. Returns nonzero when the shared cancel flag is set, and
 * pumps the spinner on the way. Usable as AgentConfig.is_cancelled and
 * ToolContext.is_cancelled (pass the Ui pointer as userdata). */
int ui_poll(void *userdata);

/* Full-screen backend. ui_start_fullscreen() returns 0 on success and cleans
 * up on failure; ui_stop_fullscreen() is idempotent and restores the
 * terminal. */
int ui_start_fullscreen(Ui *ui, const char *model, const char *protocol,
                        const char *session_id);
void ui_stop_fullscreen(Ui *ui);
int ui_is_fullscreen(const Ui *ui);
UiInputResult ui_read_input(Ui *ui, char **out);
ToolDecision ui_approve(Ui *ui, const char *tool_name,
                        const char *arguments_json);
void ui_message(Ui *ui, const char *text);
int ui_failed(const Ui *ui);
int ui_run_ended(const Ui *ui);

/* Interactive helpers. ui_prompt() prints "Input: " when a status sink exists.
 * ui_clear_status() removes the live status line before direct terminal output
 * (for example the tool-approval prompt). */
void ui_prompt(Ui *ui);
void ui_clear_status(Ui *ui);
void ui_set_phase(Ui *ui, const char *phase);

/* Answer helpers. ui_finish_stream() terminates streamed stdout;
 * ui_print_answer writes a non-streamed answer and returns 0 on success. */
int ui_finish_stream(Ui *ui);
int ui_print_answer(const char *text);

/* Result accessors for --json. */
int ui_rounds(const Ui *ui);
int ui_streamed(const Ui *ui);

/* Clears per-run state before a new agent_run(). */
void ui_reset_run(Ui *ui);

#endif /* TINY_AGENT_CLI_UI_H */
