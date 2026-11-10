#ifndef TINY_AGENT_CLI_TUI_H
#define TINY_AGENT_CLI_TUI_H

#include <signal.h>

#include "agent/config.h"
#include "agent/event.h"

/* Private ncursesw backend for the CLI. It owns the display model, the input
 * editor and the key pump; the public agent library knows nothing about it. */

typedef struct Tui Tui;

typedef enum {
  TUI_INPUT_OK = 0,
  TUI_INPUT_EOF,
  TUI_INPUT_INTERRUPTED,
  TUI_INPUT_ERROR
} TuiInputResult;

Tui *tui_create(void);
void tui_destroy(Tui *tui);

/* Returns 0 on success. On failure the terminal is restored and the caller
 * should fall back to the line-oriented UI. */
int tui_start(Tui *tui, const char *model, const char *protocol,
              const char *session_id, int quiet,
              volatile sig_atomic_t *cancel_flag,
              volatile sig_atomic_t *terminate_flag);
void tui_stop(Tui *tui);
int tui_active(const Tui *tui);
int tui_failed(const Tui *tui);

void tui_on_event(Tui *tui, const AgentEvent *event);
void tui_on_delta(Tui *tui, int round, const char *text);
int tui_poll(Tui *tui);

TuiInputResult tui_read_input(Tui *tui, char **out);
ToolDecision tui_approve(Tui *tui, const char *tool_name,
                         const char *arguments_json);
void tui_message(Tui *tui, const char *text);
void tui_set_phase(Tui *tui, const char *phase);
int tui_run_ended(const Tui *tui);
void tui_reset_run(Tui *tui);

#endif /* TINY_AGENT_CLI_TUI_H */
