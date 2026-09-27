/* console.h - in-app terminal: coloured scrollback, line editing, history, tab completion, commands */
#pragma once

#include <stdbool.h>

#include "common.h"
#include "platform.h"
#include "ui.h"

typedef void (*CommandFn)(int argc, char **argv);
/* Returns the index-th completion candidate for argument argi (1-based) starting with prefix, or NULL. */
typedef const char *(*CompleteFn)(int argi, const char *prefix, int index);

typedef struct Command {
    const char *name;
    const char *args;
    const char *help;
    CommandFn fn;
    CompleteFn complete;
} Command;

void console_init(void);
void console_register(const Command *cmds, int n);
const Command *console_find(const char *name);
int console_command_count(void);
const Command *console_command(int i);

void console_exec(const char *line, bool echo);
void console_log(LogLevel level, const char *msg); /* thread-safe; installed as the log sink */
void console_clear(void);

bool console_handle_event(const PlatformEvent *e); /* when focused; true if consumed */
void console_set_focus(bool focus);
bool console_focused(void);
void console_set_input(const char *text);
void console_scroll(float rows);

void console_draw(Ui *ui, float x, float y, float w, float h, double time);

/* helper for completion callbacks: iterate a NULL-terminated list */
const char *complete_from_list(const char *const *list, const char *prefix, int index);
