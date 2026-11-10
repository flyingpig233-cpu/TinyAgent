#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tui.h"

#define UI_TICK_MS 100

struct Ui {
  UiConfig config;
  int current_round;
  int rounds;
  int streamed;
  int streamed_needs_newline;
  int busy;
  int status_visible;
  int spinner_frame;
  int streaming; /* Suppresses the spinner once text is flowing. */
  const char *phase;
  long run_started_ms;
  long last_tick_ms;
  Tui *tui;
  int run_ended;
};

static long monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

Ui *ui_create(const UiConfig *config) {
  if (config == NULL)
    return NULL;
  Ui *ui = calloc(1, sizeof(*ui));
  if (ui == NULL)
    return NULL;
  ui->config = *config;
  ui->phase = "thinking";
  return ui;
}

void ui_destroy(Ui *ui) {
  if (ui == NULL)
    return;
  if (ui->tui != NULL)
    tui_destroy(ui->tui);
  free(ui);
}

/* ------------------------------------------------------------------ */
/* Full-screen facade                                                  */
/* ------------------------------------------------------------------ */

int ui_start_fullscreen(Ui *ui, const char *model, const char *protocol,
                        const char *session_id) {
  if (ui == NULL)
    return -1;
  if (ui->tui == NULL) {
    ui->tui = tui_create();
    if (ui->tui == NULL)
      return -1;
  }
  return tui_start(ui->tui, model, protocol, session_id, ui->config.quiet,
                   ui->config.cancel_flag, ui->config.terminate_flag);
}

void ui_stop_fullscreen(Ui *ui) {
  if (ui != NULL && ui->tui != NULL)
    tui_stop(ui->tui);
}

int ui_is_fullscreen(const Ui *ui) {
  return ui != NULL && ui->tui != NULL && tui_active(ui->tui);
}

UiInputResult ui_read_input(Ui *ui, char **out) {
  if (out != NULL)
    *out = NULL;
  if (ui == NULL || !ui_is_fullscreen(ui))
    return UI_INPUT_ERROR;
  return (UiInputResult)tui_read_input(ui->tui, out);
}

ToolDecision ui_approve(Ui *ui, const char *tool_name,
                        const char *arguments_json) {
  if (ui == NULL || !ui_is_fullscreen(ui))
    return TOOL_DECISION_DENY;
  return tui_approve(ui->tui, tool_name, arguments_json);
}

void ui_message(Ui *ui, const char *text) {
  if (ui == NULL || text == NULL)
    return;
  if (ui_is_fullscreen(ui)) {
    tui_message(ui->tui, text);
    return;
  }
  fprintf(stderr, "%s\n", text);
}

int ui_failed(const Ui *ui) {
  return ui != NULL && ui->tui != NULL && tui_failed(ui->tui);
}

int ui_run_ended(const Ui *ui) { return ui != NULL && ui->run_ended; }

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

void ui_clear_status(Ui *ui) {
  if (ui == NULL || ui_is_fullscreen(ui) || !ui->status_visible ||
      ui->config.status == NULL)
    return;
  fputs("\r\033[K", ui->config.status);
  fflush(ui->config.status);
  ui->status_visible = 0;
}

static void ui_draw_status(Ui *ui) {
  if (!ui->config.tty || ui->config.status == NULL || !ui->busy ||
      ui->streaming)
    return;
  static const char frames[] = "|/-\\";
  char frame = frames[ui->spinner_frame++ % 4];
  double elapsed = (monotonic_ms() - ui->run_started_ms) / 1000.0;
  int round = ui->current_round > 0 ? ui->current_round : 1;
  fprintf(ui->config.status, "\r\033[K%c %s  %.1fs (round %d)", frame,
          ui->phase != NULL ? ui->phase : "working", elapsed, round);
  fflush(ui->config.status);
  ui->status_visible = 1;
}

static void ui_tick(Ui *ui) {
  if (ui == NULL)
    return;
  long now = monotonic_ms();
  if (now - ui->last_tick_ms < UI_TICK_MS)
    return;
  ui->last_tick_ms = now;
  ui_draw_status(ui);
}

void ui_set_phase(Ui *ui, const char *phase) {
  if (ui == NULL)
    return;
  if (ui_is_fullscreen(ui)) {
    tui_set_phase(ui->tui, phase);
    return;
  }
  ui->phase = phase != NULL ? phase : "working";
  ui_clear_status(ui);
  ui->last_tick_ms = 0;
  ui_tick(ui);
}

/* Terminates a streamed line before printing anything else to the terminal. */
static void ui_break_stream_line(Ui *ui) {
  if (ui->streamed_needs_newline) {
    fputc('\n', stdout);
    fflush(stdout);
    ui->streamed_needs_newline = 0;
  }
}

int ui_poll(void *userdata) {
  Ui *ui = userdata;
  if (ui == NULL)
    return 0;
  if (ui_is_fullscreen(ui)) {
    if (tui_poll(ui->tui))
      return 1;
    return ui->config.cancel_flag != NULL && *ui->config.cancel_flag != 0;
  }
  ui_tick(ui);
  return ui->config.cancel_flag != NULL && *ui->config.cancel_flag != 0;
}

/* ------------------------------------------------------------------ */
/* Transcript                                                          */
/* ------------------------------------------------------------------ */

static const char *finish_reason_name(LLMFinishReason reason) {
  switch (reason) {
  case LLM_FINISH_STOP:
    return "stop";
  case LLM_FINISH_TOOL_CALLS:
    return "tool_calls";
  case LLM_FINISH_LENGTH:
    return "length";
  case LLM_FINISH_FILTERED:
    return "filtered";
  case LLM_FINISH_REFUSED:
    return "refused";
  default:
    return "unknown";
  }
}

static void print_tool_output(FILE *out, const char *text) {
  if (text == NULL)
    return;
  size_t length = strlen(text);
  if (length <= UI_TOOL_OUTPUT_LIMIT) {
    fprintf(out, "    %s\n", text);
  } else {
    fprintf(out, "    %.*s... (%zu bytes total)\n", (int)UI_TOOL_OUTPUT_LIMIT,
            text, length);
  }
}

static void print_round_header(Ui *ui, int round) {
  if (round <= 0 || round == ui->current_round)
    return;
  ui->current_round = round;
  fprintf(ui->config.transcript, "\n--- round %d ---\n", round);
}

void ui_on_event(void *userdata, const AgentEvent *event) {
  Ui *ui = userdata;
  if (ui == NULL || event == NULL)
    return;

  /* Track rounds even when the transcript is suppressed, so --json can report
   * how many model rounds the run took. */
  if (event->type == AGENT_EVENT_LLM_RESPONSE && event->round > ui->rounds)
    ui->rounds = event->round;
  if (event->type == AGENT_EVENT_RUN_END)
    ui->run_ended = 1;

  if (ui_is_fullscreen(ui)) {
    tui_on_event(ui->tui, event);
    return;
  }

  FILE *out = ui->config.transcript;
  switch (event->type) {
  case AGENT_EVENT_RUN_START:
    ui->current_round = 0;
    ui->rounds = 0;
    ui->busy = 1;
    ui->streaming = 0;
    ui->phase = "thinking";
    ui->run_started_ms = monotonic_ms();
    ui->last_tick_ms = 0;
    ui_clear_status(ui);
    if (out != NULL)
      fprintf(out, "\n=== agent run ===\n");
    ui_tick(ui);
    break;
  case AGENT_EVENT_LLM_RESPONSE:
    ui->streaming = 0;
    ui->phase = "thinking";
    ui_break_stream_line(ui);
    ui_clear_status(ui);
    if (out != NULL) {
      print_round_header(ui, event->round);
      fprintf(out, "[round %d] model: %s", event->round,
              finish_reason_name(event->finish_reason));
      if (event->finish_reason == LLM_FINISH_TOOL_CALLS)
        fprintf(out, " (%zu tool call%s)", event->tool_call_count,
                event->tool_call_count == 1 ? "" : "s");
      fprintf(out, "\n");
      /* Intermediate assistant text; the final answer is printed from the run
       * result to avoid duplication. */
      if (event->finish_reason != LLM_FINISH_STOP && event->content != NULL &&
          event->content[0] != '\0')
        fprintf(out, "[round %d] assistant: %s\n", event->round,
                event->content);
    }
    ui->current_round = event->round;
    ui->last_tick_ms = 0;
    /* Only keep spinning when another round is known to follow; a final
     * response is immediately followed by RUN_END, which clears the line. */
    if (event->finish_reason == LLM_FINISH_TOOL_CALLS)
      ui_tick(ui);
    break;
  case AGENT_EVENT_TOOL_CALL:
    ui->streaming = 0;
    ui->phase = event->tool_name != NULL ? event->tool_name : "tool";
    ui_break_stream_line(ui);
    ui_clear_status(ui);
    if (out != NULL) {
      print_round_header(ui, event->round);
      fprintf(out, "[round %d] -> %s(%s)\n", event->round,
              event->tool_name != NULL ? event->tool_name : "?",
              event->arguments_json != NULL ? event->arguments_json : "");
    }
    ui->current_round = event->round;
    ui->last_tick_ms = 0;
    ui_tick(ui);
    break;
  case AGENT_EVENT_TOOL_RESULT:
    ui->streaming = 0;
    ui->phase = "thinking";
    ui_break_stream_line(ui);
    ui_clear_status(ui);
    if (out != NULL) {
      print_round_header(ui, event->round);
      fprintf(out, "[round %d] <- %s %s\n", event->round,
              event->tool_name != NULL ? event->tool_name : "?",
              event->tool_is_error ? "error" : "ok");
      print_tool_output(out, event->tool_output);
    }
    ui->current_round = event->round;
    ui->last_tick_ms = 0;
    ui_tick(ui);
    break;
  case AGENT_EVENT_RUN_END:
    ui_break_stream_line(ui);
    ui_clear_status(ui);
    ui->busy = 0;
    if (out != NULL && event->status == AGENT_OK)
      fprintf(out, "[run] complete (%d round%s)\n",
              ui->rounds > 0 ? ui->rounds : 1, ui->rounds == 1 ? "" : "s");
    break;
  default:
    break;
  }
  if (out != NULL)
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

void ui_on_delta(void *userdata, int round, const char *text) {
  Ui *ui = userdata;
  if (ui == NULL)
    return;
  if (ui_is_fullscreen(ui)) {
    tui_on_delta(ui->tui, round, text);
    return;
  }
  if (!ui->config.stream_to_stdout || text == NULL || text[0] == '\0')
    return;
  if (!ui->streaming) {
    ui_clear_status(ui);
    ui->streaming = 1; /* Hide the spinner while text flows. */
  }
  if (fputs(text, stdout) == EOF)
    return;
  fflush(stdout);
  ui->streamed = 1;
  ui->streamed_needs_newline = text[strlen(text) - 1] != '\n';
}

int ui_finish_stream(Ui *ui) {
  if (ui == NULL)
    return -1;
  if (ui_is_fullscreen(ui))
    return 0;
  if (ui->streamed_needs_newline) {
    if (fputc('\n', stdout) == EOF)
      return -1;
    ui->streamed_needs_newline = 0;
  }
  return fflush(stdout) == EOF ? -1 : 0;
}

int ui_print_answer(const char *text) {
  if (text == NULL)
    text = "";
  if (fputs(text, stdout) == EOF)
    return -1;
  size_t length = strlen(text);
  if (length == 0 || text[length - 1] != '\n') {
    if (fputc('\n', stdout) == EOF)
      return -1;
  }
  return fflush(stdout) == EOF ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */

void ui_prompt(Ui *ui) {
  if (ui == NULL || ui_is_fullscreen(ui) || ui->config.status == NULL)
    return;
  fputs("Input: ", ui->config.status);
  fflush(ui->config.status);
}

void ui_reset_run(Ui *ui) {
  if (ui == NULL)
    return;
  ui->run_ended = 0;
  if (ui_is_fullscreen(ui))
    tui_reset_run(ui->tui);
  ui->streamed = 0;
  ui->streamed_needs_newline = 0;
  ui->streaming = 0;
  ui->current_round = 0;
  ui->rounds = 0;
}

int ui_rounds(const Ui *ui) { return ui != NULL ? ui->rounds : 0; }
int ui_streamed(const Ui *ui) { return ui != NULL ? ui->streamed : 0; }
