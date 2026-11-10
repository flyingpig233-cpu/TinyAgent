#define _POSIX_C_SOURCE 200809L
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include "tui.h"

#include "ui.h"

#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "agent/infra.h"

#define TUI_INPUT_ROWS 3
#define TUI_MIN_COLS 24
#define TUI_MIN_ROWS 8
#define TUI_DRAW_MS 33
#define TUI_STATUS_MS 100
#define TUI_INPUT_WAIT_MS 50
#define TUI_MAX_KEYS 64
#define TUI_HISTORY_LIMIT 100
#define TUI_REPLACEMENT 0xFFFDu

#define TUI_KEY_CTRL_END (KEY_MAX + 1)
#define TUI_KEY_PASTE_START (KEY_MAX + 2)
#define TUI_KEY_PASTE_END (KEY_MAX + 3)

typedef enum { MSG_USER = 0, MSG_ASSISTANT, MSG_TOOL, MSG_SYSTEM } MessageKind;

typedef struct {
  char *text;
} WrappedLine;

typedef struct {
  MessageKind kind;
  int round;
  int final;
  String text;
  WrappedLine *lines;
  size_t line_count;
  size_t line_cap;
  int wrap_dirty;
} TuiMessage;

typedef struct {
  char *text;
} InputLine;

typedef struct {
  InputLine *lines;
  size_t count;
  size_t cap;
  size_t cursor_row;
  int cursor_col;
} InputLayout;

typedef enum { EDIT_NONE = 0, EDIT_SUBMIT, EDIT_EOF } EditAction;

struct Tui {
  SCREEN *screen;
  int active;
  int failed;
  int quiet;

  int busy;
  int run_ended;
  int current_round;
  int dirty;
  int status_dirty;
  int force_status;
  int spinner_frame;
  long run_started_ms;
  long last_draw_ms;
  long last_status_ms;
  int rows;
  int cols;

  String header;
  String phase;

  String input;
  size_t cursor;
  String draft;
  int history_browsing;
  size_t history_pos;
  char **history;
  size_t history_count;
  size_t history_cap;

  TuiMessage *messages;
  size_t message_count;
  size_t message_cap;
  size_t run_start_message;

  int follow_bottom;
  size_t anchor_msg;
  size_t anchor_line;

  int approval_active;
  String approval_tool;
  String approval_args;
  int pasting;
  int cancel_requested;

  volatile sig_atomic_t *cancel_flag;
  volatile sig_atomic_t *terminate_flag;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static long monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static int string_append_bytes(String *str, const char *data, size_t length) {
  if (length == 0)
    return 0;
  if (string_reserve(str, str->length + length) != AGENT_OK)
    return -1;
  memcpy(str->data + str->length, data, length);
  str->length += length;
  str->data[str->length] = '\0';
  return 0;
}

/* Returns 1 for a complete codepoint, 0 for an incomplete trailing sequence
 * and -1 for invalid input. On -1 the caller emits a replacement character. */
static int utf8_decode(const unsigned char *text, size_t length,
                       size_t *consumed, uint32_t *codepoint) {
  if (length == 0)
    return 0;
  unsigned char first = text[0];
  if (first < 0x80) {
    *consumed = 1;
    *codepoint = first;
    return 1;
  }

  int need = 0;
  uint32_t value = 0;
  uint32_t minimum = 0;
  if ((first & 0xE0) == 0xC0) {
    need = 1;
    value = first & 0x1F;
    minimum = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    need = 2;
    value = first & 0x0F;
    minimum = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    need = 3;
    value = first & 0x07;
    minimum = 0x10000;
  } else {
    *consumed = 1;
    *codepoint = TUI_REPLACEMENT;
    return -1;
  }

  if (length < (size_t)need + 1)
    return 0;
  for (int i = 1; i <= need; ++i) {
    if ((text[i] & 0xC0) != 0x80) {
      *consumed = 1;
      *codepoint = TUI_REPLACEMENT;
      return -1;
    }
    value = (value << 6) | (text[i] & 0x3F);
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    *consumed = 1;
    *codepoint = TUI_REPLACEMENT;
    return -1;
  }
  *consumed = (size_t)need + 1;
  *codepoint = value;
  return 1;
}

static int utf8_encode(uint32_t codepoint, char out[4]) {
  if (codepoint < 0x80) {
    out[0] = (char)codepoint;
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = (char)(0xC0 | (codepoint >> 6));
    out[1] = (char)(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = (char)(0xE0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (char)(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (codepoint >> 18));
  out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = (char)(0x80 | (codepoint & 0x3F));
  return 4;
}

static size_t utf8_prev(const char *text, size_t position) {
  if (position == 0)
    return 0;
  size_t index = position - 1;
  while (index > 0 && ((unsigned char)text[index] & 0xC0) == 0x80)
    --index;
  return index;
}

static size_t utf8_next(const char *text, size_t length, size_t position) {
  if (position >= length)
    return length;
  size_t consumed = 0;
  uint32_t codepoint = 0;
  int result = utf8_decode((const unsigned char *)text + position,
                           length - position, &consumed, &codepoint);
  if (result == 0)
    return length;
  if (result < 0 || consumed == 0)
    return position + 1;
  return position + consumed;
}

/* Longest valid UTF-8 prefix no longer than limit bytes. */
static size_t utf8_prefix(const char *text, size_t length, size_t limit) {
  if (length <= limit)
    return length;
  size_t position = 0;
  size_t last = 0;
  while (position < length) {
    size_t consumed = 0;
    uint32_t codepoint = 0;
    int result = utf8_decode((const unsigned char *)text + position,
                             length - position, &consumed, &codepoint);
    if (result == 0)
      break;
    if (result < 0 || consumed == 0)
      consumed = 1;
    if (position + consumed > limit)
      break;
    position += consumed;
    last = position;
  }
  return last;
}

static void free_wrapped(WrappedLine **lines, size_t *count, size_t *cap) {
  if (*lines == NULL) {
    *count = 0;
    *cap = 0;
    return;
  }
  for (size_t i = 0; i < *count; ++i)
    free((*lines)[i].text);
  free(*lines);
  *lines = NULL;
  *count = 0;
  *cap = 0;
}

static int push_wrapped(WrappedLine **lines, size_t *count, size_t *cap,
                        String *line) {
  if (*count == *cap) {
    size_t grown = *cap == 0 ? 16 : *cap * 2;
    WrappedLine *resized = realloc(*lines, grown * sizeof(**lines));
    if (resized == NULL)
      return -1;
    *lines = resized;
    *cap = grown;
  }
  char *copy = strdup(string_c_str(line));
  if (copy == NULL)
    return -1;
  (*lines)[*count].text = copy;
  ++*count;
  string_clear(line);
  return 0;
}

static int wrap_text(const char *text, int final, int width,
                     WrappedLine **out_lines, size_t *out_count) {
  if (width < 1)
    width = 1;
  WrappedLine *lines = NULL;
  size_t count = 0;
  size_t cap = 0;
  String line = {0};
  size_t length = strlen(text);
  size_t position = 0;
  int columns = 0;

  while (position < length) {
    size_t consumed = 0;
    uint32_t codepoint = 0;
    int result = utf8_decode((const unsigned char *)text + position,
                             length - position, &consumed, &codepoint);
    if (result == 0) {
      if (!final)
        break;
      codepoint = TUI_REPLACEMENT;
      consumed = length - position;
    } else if (result < 0) {
      codepoint = TUI_REPLACEMENT;
      if (consumed == 0)
        consumed = 1;
    }
    position += consumed;

    if (codepoint == '\n') {
      if (push_wrapped(&lines, &count, &cap, &line) != 0)
        goto oom;
      columns = 0;
      continue;
    }
    if (codepoint == '\t') {
      int spaces = 4 - (columns % 4);
      for (int i = 0; i < spaces; ++i) {
        if (columns + 1 > width) {
          if (push_wrapped(&lines, &count, &cap, &line) != 0)
            goto oom;
          columns = 0;
        }
        if (string_append_bytes(&line, " ", 1) != 0)
          goto oom;
        ++columns;
      }
      continue;
    }
    if (codepoint < 0x20 || codepoint == 0x7F)
      codepoint = TUI_REPLACEMENT;

    int char_width = wcwidth((wchar_t)codepoint);
    if (char_width < 0) {
      codepoint = TUI_REPLACEMENT;
      char_width = 1;
    }
    if (char_width > width) {
      codepoint = TUI_REPLACEMENT;
      char_width = 1;
    }
    if (char_width > 0 && columns + char_width > width && columns > 0) {
      if (push_wrapped(&lines, &count, &cap, &line) != 0)
        goto oom;
      columns = 0;
    }
    char encoded[4];
    int encoded_length = utf8_encode(codepoint, encoded);
    if (string_append_bytes(&line, encoded, (size_t)encoded_length) != 0)
      goto oom;
    columns += char_width;
  }

  if (line.length > 0 || count == 0) {
    if (push_wrapped(&lines, &count, &cap, &line) != 0)
      goto oom;
  }

  string_destroy(&line);
  *out_lines = lines;
  *out_count = count;
  return 0;

oom:
  string_destroy(&line);
  free_wrapped(&lines, &count, &cap);
  return -1;
}

static void input_layout_free(InputLayout *layout) {
  for (size_t i = 0; i < layout->count; ++i)
    free(layout->lines[i].text);
  free(layout->lines);
  *layout = (InputLayout){0};
}

static int input_push(InputLayout *layout, String *line) {
  if (layout->count == layout->cap) {
    size_t grown = layout->cap == 0 ? 8 : layout->cap * 2;
    InputLine *resized = realloc(layout->lines, grown * sizeof(*layout->lines));
    if (resized == NULL)
      return -1;
    layout->lines = resized;
    layout->cap = grown;
  }
  char *copy = strdup(string_c_str(line));
  if (copy == NULL)
    return -1;
  layout->lines[layout->count].text = copy;
  ++layout->count;
  string_clear(line);
  return 0;
}

static int build_input_layout(const char *text, size_t length, size_t cursor,
                              int width, InputLayout *out) {
  if (width < 1)
    width = 1;
  *out = (InputLayout){0};
  String line = {0};
  size_t row = 0;
  int columns = 0;
  int have_cursor = 0;
  size_t position = 0;

#define RECORD_CURSOR(pos)                                                     \
  do {                                                                         \
    if (!have_cursor && (pos) == cursor) {                                     \
      out->cursor_row = row;                                                   \
      out->cursor_col = columns;                                               \
      have_cursor = 1;                                                         \
    }                                                                          \
  } while (0)

  RECORD_CURSOR(0);
  while (position < length) {
    size_t consumed = 0;
    uint32_t codepoint = 0;
    int result = utf8_decode((const unsigned char *)text + position,
                             length - position, &consumed, &codepoint);
    if (result == 0) {
      codepoint = TUI_REPLACEMENT;
      consumed = length - position;
    } else if (result < 0) {
      codepoint = TUI_REPLACEMENT;
      if (consumed == 0)
        consumed = 1;
    }
    RECORD_CURSOR(position);

    if (codepoint == '\n') {
      if (input_push(out, &line) != 0)
        goto oom;
      ++row;
      columns = 0;
    } else if (codepoint == '\t') {
      int spaces = 4 - (columns % 4);
      for (int i = 0; i < spaces; ++i) {
        if (columns + 1 > width) {
          if (input_push(out, &line) != 0)
            goto oom;
          ++row;
          columns = 0;
        }
        if (string_append_bytes(&line, " ", 1) != 0)
          goto oom;
        ++columns;
      }
    } else {
      if (codepoint < 0x20 || codepoint == 0x7F)
        codepoint = TUI_REPLACEMENT;
      int char_width = wcwidth((wchar_t)codepoint);
      if (char_width < 0) {
        codepoint = TUI_REPLACEMENT;
        char_width = 1;
      }
      if (char_width > width) {
        codepoint = TUI_REPLACEMENT;
        char_width = 1;
      }
      if (char_width > 0 && columns + char_width > width && columns > 0) {
        if (input_push(out, &line) != 0)
          goto oom;
        ++row;
        columns = 0;
      }
      char encoded[4];
      int encoded_length = utf8_encode(codepoint, encoded);
      if (string_append_bytes(&line, encoded, (size_t)encoded_length) != 0)
        goto oom;
      columns += char_width;
    }

    position += consumed;
    RECORD_CURSOR(position);
  }

  if (line.length > 0 || out->count == 0 || out->count <= row) {
    if (input_push(out, &line) != 0)
      goto oom;
  }
  if (!have_cursor) {
    out->cursor_row = row;
    out->cursor_col = columns;
  }
  string_destroy(&line);
  return 0;

oom:
  string_destroy(&line);
  input_layout_free(out);
  return -1;
#undef RECORD_CURSOR
}

/* ------------------------------------------------------------------ */
/* Message model                                                       */
/* ------------------------------------------------------------------ */

static TuiMessage *message_add(Tui *tui, MessageKind kind, int round) {
  if (tui->message_count == tui->message_cap) {
    size_t grown = tui->message_cap == 0 ? 16 : tui->message_cap * 2;
    TuiMessage *resized =
        realloc(tui->messages, grown * sizeof(*tui->messages));
    if (resized == NULL) {
      tui->failed = 1;
      return NULL;
    }
    tui->messages = resized;
    tui->message_cap = grown;
  }
  TuiMessage *message = &tui->messages[tui->message_count++];
  memset(message, 0, sizeof(*message));
  message->kind = kind;
  message->round = round;
  message->wrap_dirty = 1;
  tui->dirty = 1;
  return message;
}

static void message_destroy(TuiMessage *message) {
  string_destroy(&message->text);
  free_wrapped(&message->lines, &message->line_count, &message->line_cap);
}

static TuiMessage *find_assistant(Tui *tui, int round) {
  for (size_t i = tui->message_count; i > tui->run_start_message; --i) {
    TuiMessage *message = &tui->messages[i - 1];
    if (message->kind == MSG_ASSISTANT && message->round == round)
      return message;
  }
  return NULL;
}

static int message_set(TuiMessage *message, const char *prefix,
                       const char *body) {
  string_clear(&message->text);
  if (prefix != NULL && string_append(&message->text, prefix) != AGENT_OK)
    return -1;
  if (body != NULL && string_append(&message->text, body) != AGENT_OK)
    return -1;
  message->wrap_dirty = 1;
  return 0;
}

static int message_append(TuiMessage *message, const char *text) {
  if (string_append(&message->text, text) != AGENT_OK)
    return -1;
  message->wrap_dirty = 1;
  return 0;
}

static int ensure_wrapped(Tui *tui) {
  for (size_t i = 0; i < tui->message_count; ++i) {
    TuiMessage *message = &tui->messages[i];
    if (!message->wrap_dirty)
      continue;
    free_wrapped(&message->lines, &message->line_count, &message->line_cap);
    if (wrap_text(string_c_str(&message->text), message->final, tui->cols,
                  &message->lines, &message->line_count) != 0) {
      tui->failed = 1;
      return -1;
    }
    message->wrap_dirty = 0;
  }
  return 0;
}

static size_t total_lines(const Tui *tui) {
  size_t total = 0;
  for (size_t i = 0; i < tui->message_count; ++i)
    total += tui->messages[i].line_count;
  return total;
}

static size_t anchor_absolute(const Tui *tui) {
  size_t absolute = 0;
  for (size_t i = 0; i < tui->message_count; ++i) {
    const TuiMessage *message = &tui->messages[i];
    if (i == tui->anchor_msg) {
      size_t line = tui->anchor_line;
      if (message->line_count == 0)
        return absolute;
      if (line >= message->line_count)
        line = message->line_count - 1;
      return absolute + line;
    }
    absolute += message->line_count;
  }
  return absolute;
}

static void set_anchor_from_top(Tui *tui, size_t top) {
  size_t absolute = 0;
  for (size_t i = 0; i < tui->message_count; ++i) {
    size_t count = tui->messages[i].line_count;
    if (top < absolute + count) {
      tui->anchor_msg = i;
      tui->anchor_line = top - absolute;
      return;
    }
    absolute += count;
  }
  if (tui->message_count == 0) {
    tui->anchor_msg = 0;
    tui->anchor_line = 0;
  } else {
    tui->anchor_msg = tui->message_count - 1;
    tui->anchor_line = tui->messages[tui->message_count - 1].line_count;
  }
}

/* ------------------------------------------------------------------ */
/* Terminal drawing                                                    */
/* ------------------------------------------------------------------ */

static int too_small(const Tui *tui) {
  return tui->cols < TUI_MIN_COLS || tui->rows < TUI_MIN_ROWS;
}

static void draw_clipped(int y, int x, int max_width, const char *text,
                         attr_t attr) {
  if (max_width <= 0)
    return;
  wmove(stdscr, y, x);
  if (attr != 0)
    wattron(stdscr, attr);
  size_t length = strlen(text);
  size_t position = 0;
  int columns = 0;
  while (position < length && columns < max_width) {
    size_t consumed = 0;
    uint32_t codepoint = 0;
    int result = utf8_decode((const unsigned char *)text + position,
                             length - position, &consumed, &codepoint);
    if (result == 0) {
      codepoint = TUI_REPLACEMENT;
      consumed = length - position;
    } else if (result < 0) {
      codepoint = TUI_REPLACEMENT;
      if (consumed == 0)
        consumed = 1;
    }
    position += consumed;

    if (codepoint == '\n')
      break;
    if (codepoint == '\t') {
      int spaces = 4 - (columns % 4);
      for (int i = 0; i < spaces && columns < max_width; ++i) {
        waddch(stdscr, ' ');
        ++columns;
      }
      continue;
    }
    if (codepoint < 0x20 || codepoint == 0x7F)
      codepoint = TUI_REPLACEMENT;
    int char_width = wcwidth((wchar_t)codepoint);
    if (char_width < 0) {
      codepoint = TUI_REPLACEMENT;
      char_width = 1;
    }
    if (char_width > max_width - columns) {
      if (char_width <= 1)
        break;
      codepoint = TUI_REPLACEMENT;
      char_width = 1;
      if (char_width > max_width - columns)
        break;
    }
    char encoded[4];
    int encoded_length = utf8_encode(codepoint, encoded);
    waddnstr(stdscr, encoded, encoded_length);
    columns += char_width;
  }
  if (attr != 0)
    wattroff(stdscr, attr);
}

static void draw_row(int y, const char *text, attr_t attr) {
  wmove(stdscr, y, 0);
  wclrtoeol(stdscr);
  draw_clipped(y, 0, (int)COLS, text, attr);
}

static void clear_row(int y) {
  wmove(stdscr, y, 0);
  wclrtoeol(stdscr);
}

static void draw_header(Tui *tui) {
  draw_row(0, string_c_str(&tui->header), A_BOLD);
}

static void draw_conversation(Tui *tui) {
  int conversation_height = tui->rows - 6;
  if (conversation_height < 1)
    return;
  size_t total = total_lines(tui);
  size_t max_top = total > (size_t)conversation_height
                       ? total - (size_t)conversation_height
                       : 0;
  size_t top = 0;
  if (tui->follow_bottom) {
    top = max_top;
  } else {
    top = anchor_absolute(tui);
    if (top > max_top)
      top = max_top;
  }

  size_t line_index = 0;
  int drawn = 0;
  for (size_t i = 0; i < tui->message_count && drawn < conversation_height;
       ++i) {
    const TuiMessage *message = &tui->messages[i];
    for (size_t j = 0; j < message->line_count; ++j, ++line_index) {
      if (line_index < top)
        continue;
      draw_row(1 + drawn, message->lines[j].text, A_NORMAL);
      ++drawn;
      if (drawn >= conversation_height)
        break;
    }
  }
  for (int y = 1 + drawn; y < 1 + conversation_height; ++y)
    clear_row(y);
}

static void draw_status(Tui *tui) {
  char text[256];
  if (too_small(tui)) {
    snprintf(text, sizeof(text), "Terminal too small");
  } else if (tui->failed) {
    snprintf(text, sizeof(text), "UI error");
  } else if (tui->approval_active) {
    snprintf(text, sizeof(text), "approval required");
  } else if (tui->busy) {
    static const char frames[] = "|/-\\";
    char frame = frames[tui->spinner_frame % 4];
    double elapsed = (monotonic_ms() - tui->run_started_ms) / 1000.0;
    int round = tui->current_round > 0 ? tui->current_round : 1;
    snprintf(text, sizeof(text), "%c %.40s  %.1fs (round %d)", frame,
             string_c_str(&tui->phase), elapsed, round);
  } else {
    snprintf(text, sizeof(text), "ready");
  }
  draw_row(tui->rows - 5, text, A_REVERSE);
}

static void draw_help(Tui *tui) {
  const char *help =
      tui->approval_active
          ? "[y] allow / [n] deny / [a] allow all"
          : "Enter send | Ctrl-J newline | Up/Down history | PgUp/PgDn scroll "
            "| Ctrl-End bottom | Ctrl-C cancel | Ctrl-D exit";
  draw_row(tui->rows - 1, help, A_NORMAL);
}

static void draw_approval(Tui *tui) {
  String line = {0};
  if (string_assign(&line, "[tool] ") != AGENT_OK ||
      string_append(&line, string_c_str(&tui->approval_tool)) != AGENT_OK) {
    tui->failed = 1;
    string_destroy(&line);
    return;
  }
  draw_row(tui->rows - 4, string_c_str(&line), A_BOLD);
  string_destroy(&line);

  const char *arguments = string_c_str(&tui->approval_args);
  size_t length = strlen(arguments);
  if (length > (size_t)tui->cols) {
    size_t cut = utf8_prefix(arguments, length, (size_t)tui->cols - 3);
    String preview = {0};
    if (string_append_bytes(&preview, arguments, cut) != 0 ||
        string_append(&preview, "...") != AGENT_OK) {
      tui->failed = 1;
      string_destroy(&preview);
      return;
    }
    draw_row(tui->rows - 3, string_c_str(&preview), A_NORMAL);
    string_destroy(&preview);
  } else {
    draw_row(tui->rows - 3, arguments, A_NORMAL);
  }

  draw_row(tui->rows - 2, "[y] allow / [n] deny / [a] allow all", A_NORMAL);
}

static void draw_input(Tui *tui) {
  if (tui->approval_active) {
    draw_approval(tui);
    curs_set(0);
    return;
  }

  InputLayout layout = {0};
  int width = tui->cols - 2;
  if (width < 1)
    width = 1;
  if (build_input_layout(string_c_str(&tui->input), tui->input.length,
                         tui->cursor, width, &layout) != 0) {
    tui->failed = 1;
    return;
  }

  size_t first = 0;
  if (layout.cursor_row >= TUI_INPUT_ROWS)
    first = layout.cursor_row - (TUI_INPUT_ROWS - 1);
  if (layout.count > 0 && first >= layout.count)
    first = layout.count - 1;

  for (int row = 0; row < TUI_INPUT_ROWS; ++row) {
    int y = tui->rows - 4 + row;
    clear_row(y);
    size_t index = first + (size_t)row;
    if (index >= layout.count)
      continue;
    if (index == 0) {
      wmove(stdscr, y, 0);
      waddstr(stdscr, "> ");
      draw_clipped(y, 2, tui->cols - 2, layout.lines[index].text, A_NORMAL);
    } else {
      draw_clipped(y, 0, tui->cols, layout.lines[index].text, A_NORMAL);
    }
  }

  int cursor_y = tui->rows - 4 + (int)(layout.cursor_row - first);
  int cursor_x = layout.cursor_col + (layout.cursor_row == 0 ? 2 : 0);
  if (cursor_y >= tui->rows - 4 && cursor_y <= tui->rows - 2 && cursor_x >= 0 &&
      cursor_x < tui->cols) {
    wmove(stdscr, cursor_y, cursor_x);
    curs_set(tui->busy ? 0 : 1);
  } else {
    curs_set(0);
  }
  input_layout_free(&layout);
}

static void draw_too_small(Tui *tui) {
  werase(stdscr);
  const char *first = "Terminal too small";
  const char *second = "Need at least 24x8";
  int y = tui->rows / 2;
  int x1 = (tui->cols - (int)strlen(first)) / 2;
  int x2 = (tui->cols - (int)strlen(second)) / 2;
  if (x1 < 0)
    x1 = 0;
  if (x2 < 0)
    x2 = 0;
  mvwaddstr(stdscr, y, x1, first);
  if (y + 1 < tui->rows)
    mvwaddstr(stdscr, y + 1, x2, second);
  curs_set(0);
}

static void render(Tui *tui) {
  if (!tui->active)
    return;
  int rows = 0;
  int cols = 0;
  getmaxyx(stdscr, rows, cols);
  if (rows != tui->rows || cols != tui->cols) {
    tui->rows = rows;
    tui->cols = cols;
    for (size_t i = 0; i < tui->message_count; ++i)
      tui->messages[i].wrap_dirty = 1;
    tui->dirty = 1;
  }
  if (tui->rows < 1 || tui->cols < 1)
    return;

  if (too_small(tui)) {
    draw_too_small(tui);
    wnoutrefresh(stdscr);
    doupdate();
    tui->dirty = 0;
    tui->status_dirty = 0;
    tui->force_status = 0;
    tui->last_draw_ms = monotonic_ms();
    return;
  }

  if (ensure_wrapped(tui) != 0) {
    /* Still show the failure state below. */
  }
  draw_header(tui);
  draw_conversation(tui);
  draw_status(tui);
  draw_help(tui);
  if (tui->force_status) {
    wredrawln(stdscr, tui->rows - 5, 1);
    tui->force_status = 0;
  }
  draw_input(tui);
  wnoutrefresh(stdscr);
  doupdate();
  tui->dirty = 0;
  tui->status_dirty = 0;
  tui->last_draw_ms = monotonic_ms();
}

static void tick(Tui *tui) {
  if (!tui->active)
    return;
  long now = monotonic_ms();
  if (tui->busy && now - tui->last_status_ms >= TUI_STATUS_MS) {
    tui->last_status_ms = now;
    ++tui->spinner_frame;
    tui->status_dirty = 1;
  }
  if (tui->failed)
    tui->status_dirty = 1;
  if ((tui->dirty || tui->status_dirty || tui->force_status) &&
      (tui->last_draw_ms == 0 || now - tui->last_draw_ms >= TUI_DRAW_MS))
    render(tui);
}

static void layout_changed(Tui *tui) {
  for (size_t i = 0; i < tui->message_count; ++i)
    tui->messages[i].wrap_dirty = 1;
  tui->dirty = 1;
  tui->status_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Input editing                                                       */
/* ------------------------------------------------------------------ */

static void input_clear(Tui *tui) {
  string_clear(&tui->input);
  tui->cursor = 0;
  tui->history_browsing = 0;
  string_clear(&tui->draft);
  tui->dirty = 1;
}

static void input_insert_bytes(Tui *tui, const char *data, size_t length) {
  if (length == 0)
    return;
  if (tui->cursor > tui->input.length)
    tui->cursor = tui->input.length;
  if (string_reserve(&tui->input, tui->input.length + length) != AGENT_OK) {
    tui->failed = 1;
    return;
  }
  memmove(tui->input.data + tui->cursor + length, tui->input.data + tui->cursor,
          tui->input.length - tui->cursor + 1);
  memcpy(tui->input.data + tui->cursor, data, length);
  tui->input.length += length;
  tui->cursor += length;
  tui->history_browsing = 0;
  tui->dirty = 1;
}

static void input_insert_codepoint(Tui *tui, uint32_t codepoint) {
  char encoded[4];
  int length = utf8_encode(codepoint, encoded);
  input_insert_bytes(tui, encoded, (size_t)length);
}

static void input_delete_previous(Tui *tui) {
  if (tui->cursor == 0)
    return;
  size_t previous = utf8_prev(string_c_str(&tui->input), tui->cursor);
  size_t removed = tui->cursor - previous;
  memmove(tui->input.data + previous, tui->input.data + tui->cursor,
          tui->input.length - tui->cursor + 1);
  tui->input.length -= removed;
  tui->cursor = previous;
  tui->history_browsing = 0;
  tui->dirty = 1;
}

static void input_delete_next(Tui *tui) {
  if (tui->cursor >= tui->input.length)
    return;
  size_t next =
      utf8_next(string_c_str(&tui->input), tui->input.length, tui->cursor);
  size_t removed = next - tui->cursor;
  if (removed == 0)
    return;
  memmove(tui->input.data + tui->cursor, tui->input.data + next,
          tui->input.length - next + 1);
  tui->input.length -= removed;
  tui->history_browsing = 0;
  tui->dirty = 1;
}

static void input_move_left(Tui *tui) {
  if (tui->cursor > 0) {
    tui->cursor = utf8_prev(string_c_str(&tui->input), tui->cursor);
    tui->dirty = 1;
  }
}

static void input_move_right(Tui *tui) {
  if (tui->cursor < tui->input.length) {
    tui->cursor =
        utf8_next(string_c_str(&tui->input), tui->input.length, tui->cursor);
    tui->dirty = 1;
  }
}

static void input_home(Tui *tui) {
  size_t position = tui->cursor;
  while (position > 0 && tui->input.data[position - 1] != '\n')
    --position;
  tui->cursor = position;
  tui->dirty = 1;
}

static void input_end(Tui *tui) {
  size_t position = tui->cursor;
  while (position < tui->input.length && tui->input.data[position] != '\n')
    ++position;
  tui->cursor = position;
  tui->dirty = 1;
}

static int input_is_blank(const Tui *tui) {
  for (size_t i = 0; i < tui->input.length; ++i) {
    unsigned char c = (unsigned char)tui->input.data[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      return 0;
  }
  return 1;
}

static int history_add(Tui *tui, const char *text) {
  if (tui->history_count > 0 &&
      strcmp(tui->history[tui->history_count - 1], text) == 0)
    return 0;
  if (tui->history_count == TUI_HISTORY_LIMIT) {
    free(tui->history[0]);
    memmove(tui->history, tui->history + 1,
            (tui->history_count - 1) * sizeof(*tui->history));
    --tui->history_count;
  }
  if (tui->history_count == tui->history_cap) {
    size_t grown = tui->history_cap == 0 ? 16 : tui->history_cap * 2;
    char **resized = realloc(tui->history, grown * sizeof(*tui->history));
    if (resized == NULL)
      return -1;
    tui->history = resized;
    tui->history_cap = grown;
  }
  char *copy = strdup(text);
  if (copy == NULL)
    return -1;
  tui->history[tui->history_count++] = copy;
  tui->history_pos = tui->history_count;
  tui->history_browsing = 0;
  return 0;
}

static void history_up(Tui *tui) {
  if (tui->history_count == 0)
    return;
  if (!tui->history_browsing) {
    if (string_assign(&tui->draft, string_c_str(&tui->input)) != AGENT_OK) {
      tui->failed = 1;
      return;
    }
    tui->history_browsing = 1;
    tui->history_pos = tui->history_count;
  }
  if (tui->history_pos > 0)
    --tui->history_pos;
  if (string_assign(&tui->input, tui->history[tui->history_pos]) != AGENT_OK) {
    tui->failed = 1;
    return;
  }
  tui->cursor = tui->input.length;
  tui->dirty = 1;
}

static void history_down(Tui *tui) {
  if (!tui->history_browsing)
    return;
  if (tui->history_pos + 1 < tui->history_count) {
    ++tui->history_pos;
    if (string_assign(&tui->input, tui->history[tui->history_pos]) !=
        AGENT_OK) {
      tui->failed = 1;
      return;
    }
  } else {
    tui->history_browsing = 0;
    if (string_assign(&tui->input, string_c_str(&tui->draft)) != AGENT_OK) {
      tui->failed = 1;
      return;
    }
    string_clear(&tui->draft);
  }
  tui->cursor = tui->input.length;
  tui->dirty = 1;
}

static void scroll_page(Tui *tui, int direction) {
  int conversation_height = tui->rows - 6;
  if (conversation_height < 1)
    return;
  size_t total = total_lines(tui);
  size_t max_top = total > (size_t)conversation_height
                       ? total - (size_t)conversation_height
                       : 0;
  size_t top = tui->follow_bottom ? max_top : anchor_absolute(tui);
  if (top > max_top)
    top = max_top;
  size_t page = (size_t)conversation_height;
  if (direction < 0) {
    top = top > page ? top - page : 0;
  } else {
    top = top + page > max_top ? max_top : top + page;
  }
  set_anchor_from_top(tui, top);
  tui->follow_bottom = top >= max_top;
  tui->dirty = 1;
}

static void scroll_end(Tui *tui) {
  tui->follow_bottom = 1;
  tui->dirty = 1;
}

static void insert_pasted_codepoint(Tui *tui, uint32_t codepoint) {
  if (codepoint == '\r' || codepoint == '\n')
    codepoint = '\n';
  input_insert_codepoint(tui, codepoint);
}

static EditAction handle_editor_key(Tui *tui, int result, wint_t input) {
  if (too_small(tui)) {
    if (result == OK && input == 4)
      return EDIT_EOF;
    if (result == OK && input == 3) {
      input_clear(tui);
      return EDIT_NONE;
    }
    if (result == KEY_CODE_YES && input == KEY_RESIZE)
      layout_changed(tui);
    return EDIT_NONE;
  }

  if (result == KEY_CODE_YES) {
    switch ((int)input) {
    case KEY_RESIZE:
      layout_changed(tui);
      return EDIT_NONE;
    case KEY_LEFT:
      if (!tui->pasting)
        input_move_left(tui);
      return EDIT_NONE;
    case KEY_RIGHT:
      if (!tui->pasting)
        input_move_right(tui);
      return EDIT_NONE;
    case KEY_UP:
      if (!tui->pasting)
        history_up(tui);
      return EDIT_NONE;
    case KEY_DOWN:
      if (!tui->pasting)
        history_down(tui);
      return EDIT_NONE;
    case KEY_HOME:
      if (!tui->pasting)
        input_home(tui);
      return EDIT_NONE;
    case KEY_END:
      if (!tui->pasting)
        input_end(tui);
      return EDIT_NONE;
    case KEY_BACKSPACE:
      if (!tui->pasting)
        input_delete_previous(tui);
      return EDIT_NONE;
    case KEY_DC:
      if (!tui->pasting)
        input_delete_next(tui);
      return EDIT_NONE;
    case KEY_ENTER:
      if (tui->pasting) {
        insert_pasted_codepoint(tui, '\n');
        return EDIT_NONE;
      }
      return EDIT_SUBMIT;
    case KEY_PPAGE:
      scroll_page(tui, -1);
      return EDIT_NONE;
    case KEY_NPAGE:
      scroll_page(tui, 1);
      return EDIT_NONE;
    case TUI_KEY_CTRL_END:
      scroll_end(tui);
      return EDIT_NONE;
    case TUI_KEY_PASTE_START:
      tui->pasting = 1;
      return EDIT_NONE;
    case TUI_KEY_PASTE_END:
      tui->pasting = 0;
      return EDIT_NONE;
    default:
      return EDIT_NONE;
    }
  }

  if (input == '\r') {
    if (tui->pasting) {
      insert_pasted_codepoint(tui, '\n');
      return EDIT_NONE;
    }
    return EDIT_SUBMIT;
  }
  if (input == '\n') {
    insert_pasted_codepoint(tui, '\n');
    return EDIT_NONE;
  }
  if (input == 3) {
    if (!tui->pasting)
      input_clear(tui);
    return EDIT_NONE;
  }
  if (input == 4) {
    if (tui->input.length == 0)
      return EDIT_EOF;
    input_delete_next(tui);
    return EDIT_NONE;
  }
  if (input == 127 || input == 8) {
    if (!tui->pasting)
      input_delete_previous(tui);
    return EDIT_NONE;
  }
  if (input >= 32 && input <= 0x10FFFF) {
    if (tui->pasting)
      insert_pasted_codepoint(tui, (uint32_t)input);
    else
      input_insert_codepoint(tui, (uint32_t)input);
  }
  return EDIT_NONE;
}

static void handle_busy_key(Tui *tui, int result, wint_t input) {
  if (result == KEY_CODE_YES) {
    switch ((int)input) {
    case KEY_RESIZE:
      layout_changed(tui);
      break;
    case KEY_PPAGE:
      scroll_page(tui, -1);
      break;
    case KEY_NPAGE:
      scroll_page(tui, 1);
      break;
    case TUI_KEY_CTRL_END:
      scroll_end(tui);
      break;
    default:
      break;
    }
    return;
  }
  if (input == 3) {
    if (tui->cancel_flag != NULL)
      *tui->cancel_flag = 1;
    else
      tui->cancel_requested = 1;
  }
}

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */
/* ------------------------------------------------------------------ */

Tui *tui_create(void) { return calloc(1, sizeof(Tui)); }

void tui_stop(Tui *tui) {
  if (tui == NULL || !tui->active)
    return;
  fputs("\033[?2004l", stdout);
  fflush(stdout);
  endwin();
  if (tui->screen != NULL) {
    delscreen(tui->screen);
    tui->screen = NULL;
  }
  tui->active = 0;
  tui->dirty = 0;
  tui->status_dirty = 0;
  tui->approval_active = 0;
  tui->pasting = 0;
}

void tui_destroy(Tui *tui) {
  if (tui == NULL)
    return;
  tui_stop(tui);
  for (size_t i = 0; i < tui->message_count; ++i)
    message_destroy(&tui->messages[i]);
  free(tui->messages);
  for (size_t i = 0; i < tui->history_count; ++i)
    free(tui->history[i]);
  free(tui->history);
  string_destroy(&tui->header);
  string_destroy(&tui->phase);
  string_destroy(&tui->input);
  string_destroy(&tui->draft);
  string_destroy(&tui->approval_tool);
  string_destroy(&tui->approval_args);
  free(tui);
}

int tui_start(Tui *tui, const char *model, const char *protocol,
              const char *session_id, int quiet,
              volatile sig_atomic_t *cancel_flag,
              volatile sig_atomic_t *terminate_flag) {
  if (tui == NULL)
    return -1;
  if (tui->active)
    return 0;

  if (setlocale(LC_CTYPE, "") == NULL)
    return -1;
  const char *codeset = nl_langinfo(CODESET);
  if (codeset == NULL || strcasecmp(codeset, "UTF-8") != 0)
    return -1;

  tui->screen = newterm(NULL, stdout, stdin);
  if (tui->screen == NULL)
    return -1;
  set_term(tui->screen);
  cbreak();
  noecho();
  nonl();
  keypad(stdscr, TRUE);
  intrflush(stdscr, FALSE);

  define_key("\033[1;5F", TUI_KEY_CTRL_END);
  define_key("\033[200~", TUI_KEY_PASTE_START);
  define_key("\033[201~", TUI_KEY_PASTE_END);

  tui->active = 1;
  tui->failed = 0;
  tui->quiet = quiet;
  tui->cancel_flag = cancel_flag;
  tui->terminate_flag = terminate_flag;
  tui->busy = 0;
  tui->run_ended = 0;
  tui->current_round = 0;
  tui->follow_bottom = 1;
  tui->pasting = 0;
  tui->approval_active = 0;
  tui->force_status = 0;
  tui->cancel_requested = 0;
  tui->last_draw_ms = 0;
  tui->last_status_ms = 0;

  char header[512];
  snprintf(header, sizeof(header),
           "TinyAgent  model=%.80s  protocol=%.40s  "
           "session=%.80s",
           model != NULL ? model : "?", protocol != NULL ? protocol : "?",
           session_id != NULL ? session_id : "?");
  if (string_assign(&tui->header, header) != AGENT_OK ||
      string_assign(&tui->phase, "thinking") != AGENT_OK) {
    tui->failed = 1;
    tui_stop(tui);
    tui->failed = 0; /* A failed start is reported by the return value. */
    return -1;
  }

  fputs("\033[?2004h", stdout);
  fflush(stdout);
  curs_set(0);
  tui->dirty = 1;
  render(tui);
  return 0;
}

int tui_active(const Tui *tui) { return tui != NULL && tui->active; }

int tui_failed(const Tui *tui) { return tui != NULL && tui->failed; }

/* ------------------------------------------------------------------ */
/* Agent events                                                        */
/* ------------------------------------------------------------------ */

void tui_on_event(Tui *tui, const AgentEvent *event) {
  if (tui == NULL || !tui->active || event == NULL)
    return;

  switch (event->type) {
  case AGENT_EVENT_RUN_START: {
    tui->busy = 1;
    tui->run_ended = 0;
    tui->current_round = 0;
    tui->run_start_message = tui->message_count;
    tui->follow_bottom = 1;
    tui->run_started_ms = monotonic_ms();
    tui->last_status_ms = 0;
    tui->status_dirty = 1;
    if (string_assign(&tui->phase, "thinking") != AGENT_OK)
      tui->failed = 1;
    TuiMessage *message = message_add(tui, MSG_USER, 0);
    if (message != NULL &&
        message_set(message,
                    "You: ", event->content != NULL ? event->content : "") != 0)
      tui->failed = 1;
    break;
  }
  case AGENT_EVENT_LLM_RESPONSE: {
    tui->busy = 1;
    tui->current_round = event->round;
    tui->status_dirty = 1;
    if (string_assign(&tui->phase, "thinking") != AGENT_OK)
      tui->failed = 1;
    TuiMessage *message = find_assistant(tui, event->round);
    if (event->content != NULL && event->content[0] != '\0') {
      if (message == NULL)
        message = message_add(tui, MSG_ASSISTANT, event->round);
      if (message != NULL) {
        if (message_set(message, "Agent: ", event->content) != 0)
          tui->failed = 1;
        message->final = 1;
      }
    } else if (message != NULL) {
      message->final = 1;
    } else if (event->finish_reason != LLM_FINISH_TOOL_CALLS) {
      message = message_add(tui, MSG_ASSISTANT, event->round);
      if (message != NULL) {
        if (message_set(message, "Agent: ", "") != 0)
          tui->failed = 1;
        message->final = 1;
      }
    }
    break;
  }
  case AGENT_EVENT_TOOL_CALL: {
    tui->current_round = event->round;
    if (string_assign(&tui->phase,
                      event->tool_name != NULL ? event->tool_name : "tool") !=
        AGENT_OK)
      tui->failed = 1;
    tui->status_dirty = 1;
    if (tui->quiet)
      break;
    String text = {0};
    if (string_assign(&text, "  -> ") != AGENT_OK ||
        string_append(&text, event->tool_name != NULL ? event->tool_name
                                                      : "?") != AGENT_OK ||
        string_append(&text, "(") != AGENT_OK ||
        string_append(&text, event->arguments_json != NULL
                                 ? event->arguments_json
                                 : "") != AGENT_OK ||
        string_append(&text, ")") != AGENT_OK) {
      tui->failed = 1;
      string_destroy(&text);
      break;
    }
    TuiMessage *message = message_add(tui, MSG_TOOL, event->round);
    if (message == NULL) {
      string_destroy(&text);
      break;
    }
    if (string_assign(&message->text, string_c_str(&text)) != AGENT_OK)
      tui->failed = 1;
    message->final = 1;
    message->wrap_dirty = 1;
    string_destroy(&text);
    break;
  }
  case AGENT_EVENT_TOOL_RESULT: {
    tui->current_round = event->round;
    if (string_assign(&tui->phase, "thinking") != AGENT_OK)
      tui->failed = 1;
    tui->status_dirty = 1;
    if (tui->quiet)
      break;
    String text = {0};
    if (string_assign(&text, "  <- ") != AGENT_OK ||
        string_append(&text, event->tool_name != NULL ? event->tool_name
                                                      : "?") != AGENT_OK ||
        string_append(&text, event->tool_is_error ? " error" : " ok") !=
            AGENT_OK) {
      tui->failed = 1;
      string_destroy(&text);
      break;
    }
    if (event->tool_output != NULL && event->tool_output[0] != '\0') {
      if (string_append(&text, "\n") != AGENT_OK) {
        tui->failed = 1;
        string_destroy(&text);
        break;
      }
      size_t output_length = strlen(event->tool_output);
      if (output_length <= UI_TOOL_OUTPUT_LIMIT) {
        if (string_append(&text, event->tool_output) != AGENT_OK) {
          tui->failed = 1;
          string_destroy(&text);
          break;
        }
      } else {
        size_t cut = utf8_prefix(event->tool_output, output_length,
                                 UI_TOOL_OUTPUT_LIMIT);
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "... (%zu bytes total)",
                 output_length);
        if (string_append_bytes(&text, event->tool_output, cut) != 0 ||
            string_append(&text, suffix) != AGENT_OK) {
          tui->failed = 1;
          string_destroy(&text);
          break;
        }
      }
    }
    TuiMessage *message = message_add(tui, MSG_TOOL, event->round);
    if (message == NULL) {
      string_destroy(&text);
      break;
    }
    if (string_assign(&message->text, string_c_str(&text)) != AGENT_OK)
      tui->failed = 1;
    message->final = 1;
    message->wrap_dirty = 1;
    string_destroy(&text);
    break;
  }
  case AGENT_EVENT_RUN_END: {
    tui->busy = 0;
    tui->run_ended = 1;
    tui->last_status_ms = 0;
    tui->status_dirty = 1;
    tui->force_status = 1;
    if (string_assign(&tui->phase, "ready") != AGENT_OK)
      tui->failed = 1;
    if (event->status == AGENT_OK) {
      if (event->content != NULL) {
        TuiMessage *message = find_assistant(tui, tui->current_round);
        if (message == NULL)
          message = message_add(tui, MSG_ASSISTANT, tui->current_round);
        if (message != NULL) {
          if (message_set(message, "Agent: ", event->content) != 0)
            tui->failed = 1;
          message->final = 1;
        }
      }
    } else {
      String text = {0};
      if (event->status == AGENT_ERR_CANCELLED) {
        const char *message =
            tui->failed ? "[error] terminal UI failed" : "[cancelled]";
        if (string_assign(&text, message) != AGENT_OK)
          tui->failed = 1;
      } else {
        const char *message =
            event->error_message != NULL && event->error_message[0] != '\0'
                ? event->error_message
                : "run failed";
        if (string_assign(&text, "[error] ") != AGENT_OK ||
            string_append(&text, message) != AGENT_OK)
          tui->failed = 1;
      }
      TuiMessage *system = message_add(tui, MSG_SYSTEM, 0);
      if (system != NULL) {
        if (string_assign(&system->text, string_c_str(&text)) != AGENT_OK)
          tui->failed = 1;
        system->final = 1;
        system->wrap_dirty = 1;
      }
      string_destroy(&text);
    }
    break;
  }
  default:
    break;
  }
  tui->dirty = 1;
}

void tui_on_delta(Tui *tui, int round, const char *text) {
  if (tui == NULL || !tui->active || text == NULL || text[0] == '\0')
    return;
  tui->busy = 1;
  tui->current_round = round;
  if (string_assign(&tui->phase, "responding") != AGENT_OK)
    tui->failed = 1;
  tui->status_dirty = 1;
  TuiMessage *message = find_assistant(tui, round);
  if (message == NULL) {
    message = message_add(tui, MSG_ASSISTANT, round);
    if (message == NULL)
      return;
    if (string_assign(&message->text, "Agent: ") != AGENT_OK)
      tui->failed = 1;
  }
  message->final = 0;
  if (message_append(message, text) != 0)
    tui->failed = 1;
  tui->dirty = 1;
}

int tui_poll(Tui *tui) {
  if (tui == NULL || !tui->active)
    return 0;
  if (tui->terminate_flag != NULL && *tui->terminate_flag != 0)
    return 1;
  if (tui->cancel_flag != NULL && *tui->cancel_flag != 0)
    return 1;

  wtimeout(stdscr, 0);
  for (int i = 0; i < TUI_MAX_KEYS; ++i) {
    wint_t input = 0;
    int result = wget_wch(stdscr, &input);
    if (result == ERR)
      break;
    handle_busy_key(tui, result, input);
    if (tui->cancel_requested)
      break;
  }
  tick(tui);
  if (tui->failed || tui->cancel_requested)
    return 1;
  if (tui->terminate_flag != NULL && *tui->terminate_flag != 0)
    return 1;
  if (tui->cancel_flag != NULL && *tui->cancel_flag != 0)
    return 1;
  return 0;
}

TuiInputResult tui_read_input(Tui *tui, char **out) {
  if (tui == NULL || !tui->active || out == NULL)
    return TUI_INPUT_ERROR;
  *out = NULL;
  wtimeout(stdscr, TUI_INPUT_WAIT_MS);
  for (;;) {
    if (tui->failed)
      return TUI_INPUT_ERROR;
    if (tui->terminate_flag != NULL && *tui->terminate_flag != 0)
      return TUI_INPUT_INTERRUPTED;
    if (tui->cancel_flag != NULL && *tui->cancel_flag != 0) {
      *tui->cancel_flag = 0;
      input_clear(tui);
    }
    tick(tui);

    wint_t input = 0;
    int result = wget_wch(stdscr, &input);
    if (result == ERR)
      continue;
    EditAction action = handle_editor_key(tui, result, input);
    if (action == EDIT_SUBMIT) {
      if (input_is_blank(tui)) {
        tui->dirty = 1;
        continue;
      }
      char *copy = strdup(string_c_str(&tui->input));
      if (copy == NULL) {
        tui->failed = 1;
        return TUI_INPUT_ERROR;
      }
      if (history_add(tui, string_c_str(&tui->input)) != 0) {
        tui->failed = 1;
        free(copy);
        return TUI_INPUT_ERROR;
      }
      *out = copy;
      input_clear(tui);
      return TUI_INPUT_OK;
    }
    if (action == EDIT_EOF)
      return TUI_INPUT_EOF;
    if (tui->dirty || tui->status_dirty)
      tick(tui);
  }
}

ToolDecision tui_approve(Tui *tui, const char *tool_name,
                         const char *arguments_json) {
  if (tui == NULL || !tui->active)
    return TOOL_DECISION_DENY;
  if (string_assign(&tui->approval_tool, tool_name != NULL ? tool_name : "?") !=
          AGENT_OK ||
      string_assign(&tui->approval_args,
                    arguments_json != NULL ? arguments_json : "") != AGENT_OK) {
    tui->failed = 1;
    return TOOL_DECISION_DENY;
  }
  tui->approval_active = 1;
  tui->pasting = 0;
  tui->dirty = 1;
  render(tui);

  ToolDecision decision = TOOL_DECISION_DENY;
  wtimeout(stdscr, TUI_INPUT_WAIT_MS);
  for (;;) {
    if (tui->failed)
      break;
    if (tui->terminate_flag != NULL && *tui->terminate_flag != 0)
      break;
    if (tui->cancel_flag != NULL && *tui->cancel_flag != 0)
      break;
    tick(tui);

    wint_t input = 0;
    int result = wget_wch(stdscr, &input);
    if (result == ERR)
      continue;

    if (result == KEY_CODE_YES) {
      switch ((int)input) {
      case KEY_RESIZE:
        layout_changed(tui);
        continue;
      case KEY_PPAGE:
        scroll_page(tui, -1);
        continue;
      case KEY_NPAGE:
        scroll_page(tui, 1);
        continue;
      case TUI_KEY_CTRL_END:
        scroll_end(tui);
        continue;
      case TUI_KEY_PASTE_START:
        tui->pasting = 1;
        continue;
      case TUI_KEY_PASTE_END:
        tui->pasting = 0;
        continue;
      case KEY_ENTER:
        if (!tui->pasting)
          decision = TOOL_DECISION_DENY;
        break;
      default:
        continue;
      }
      if (decision == TOOL_DECISION_DENY)
        break;
      continue;
    }

    if (tui->pasting)
      continue;
    if (input == 3) {
      if (tui->cancel_flag != NULL)
        *tui->cancel_flag = 1;
      else
        tui->cancel_requested = 1;
      decision = TOOL_DECISION_DENY;
      break;
    }
    if (input == 27 || input == '\r' || input == '\n') {
      decision = TOOL_DECISION_DENY;
      break;
    }
    if (input == 'y' || input == 'Y') {
      decision = TOOL_DECISION_ALLOW;
      break;
    }
    if (input == 'n' || input == 'N') {
      decision = TOOL_DECISION_DENY;
      break;
    }
    if (input == 'a' || input == 'A') {
      decision = TOOL_DECISION_ALLOW_ALL;
      break;
    }
  }

  tui->approval_active = 0;
  tui->dirty = 1;
  render(tui);
  return decision;
}

void tui_message(Tui *tui, const char *text) {
  if (tui == NULL || !tui->active || text == NULL)
    return;
  TuiMessage *message = message_add(tui, MSG_SYSTEM, 0);
  if (message == NULL)
    return;
  if (message_set(message, "[system] ", text) != 0)
    tui->failed = 1;
  message->final = 1;
  tui->dirty = 1;
}

void tui_set_phase(Tui *tui, const char *phase) {
  if (tui == NULL || !tui->active)
    return;
  if (string_assign(&tui->phase, phase != NULL ? phase : "working") != AGENT_OK)
    tui->failed = 1;
  tui->status_dirty = 1;
}

int tui_run_ended(const Tui *tui) { return tui != NULL && tui->run_ended; }

void tui_reset_run(Tui *tui) {
  if (tui == NULL)
    return;
  tui->run_ended = 0;
  tui->current_round = 0;
  tui->run_start_message = tui->message_count;
  tui->cancel_requested = 0;
  tui->busy = 0;
  tui->dirty = 1;
}
