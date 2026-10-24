/* platform.h - window, OpenGL 4.1 context and input (implemented in pure C on macOS/Cocoa) */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MOD_SHIFT = 1 << 0,
    MOD_CTRL = 1 << 1,
    MOD_ALT = 1 << 2,
    MOD_CMD = 1 << 3,
} PlatformMod;

/* Printable keys are reported as their lowercase ASCII code ('a', '1', '`', ...). */
typedef enum {
    KEY_NONE = 0,
    KEY_ESCAPE = 256,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
} PlatformKey;

typedef enum {
    EV_NONE = 0,
    EV_KEY_DOWN,
    EV_KEY_UP,
    EV_TEXT,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_SCROLL,
    EV_MAGNIFY,
    EV_FILE_DROP,
    EV_RESIZE,
} PlatformEventType;

typedef enum { CURSOR_ARROW = 0, CURSOR_IBEAM, CURSOR_HAND, CURSOR_CROSSHAIR, CURSOR_GRAB, CURSOR_GRABBING, CURSOR_RESIZE_H } PlatformCursor;

typedef struct PlatformEvent {
    PlatformEventType type;
    int key;            /* EV_KEY_*: PlatformKey or lowercase ASCII */
    uint32_t mods;      /* PlatformMod bits */
    bool repeat;
    uint32_t codepoint; /* EV_TEXT */
    int button;         /* 0 left, 1 right, 2 middle */
    int clicks;
    double x, y;        /* mouse position in points, origin top-left of the content area */
    double dx, dy;      /* EV_SCROLL deltas (points) */
    bool precise;       /* EV_SCROLL from a trackpad */
    double magnify;     /* EV_MAGNIFY */
    char path[1024];    /* EV_FILE_DROP */
} PlatformEvent;

/* headless: no window, just a current OpenGL 4.1 core context (render into FBOs). */
bool platform_init(const char *title, int width, int height, bool headless);
void platform_shutdown(void);

/* Non-blocking. Returns number of events written. */
int platform_poll_events(PlatformEvent *events, int max_events);
bool platform_should_quit(void);
void platform_request_quit(void);
void platform_swap(void);
void platform_set_vsync(bool on);

/* Content size in points and drawable framebuffer size in pixels. */
void platform_get_size(int *w_pts, int *h_pts, int *fb_w, int *fb_h);
double platform_backing_scale(void);
void platform_get_mouse(double *x, double *y);
bool platform_mouse_down(int button);
uint32_t platform_mods(void);
bool platform_window_focused(void);

void platform_set_title(const char *title);
void platform_set_cursor(PlatformCursor cursor);

/* Native dialogs (modal). ext like "stl" or NULL. */
bool platform_open_file_dialog(char *out_path, size_t cap, const char *ext);
bool platform_save_file_dialog(char *out_path, size_t cap, const char *default_name);

/* Clipboard (UTF-8). get returns false if empty. */
bool platform_clipboard_get(char *out, size_t cap);
void platform_clipboard_set(const char *text);

/* Reveal a file in Finder / open with default app. */
void platform_open_path(const char *path);
