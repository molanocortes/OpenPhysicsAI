/* platform_macos.c - Cocoa window + NSOpenGLContext + input, written in plain C via the Objective-C runtime */
#define GL_SILENCE_DEPRECATION
#include "platform.h"
#include "common.h"

#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

typedef CGRect NSRect;
typedef CGPoint NSPoint;
typedef CGSize NSSize;

extern id const NSDefaultRunLoopMode;
extern id const NSAppearanceNameDarkAqua;
extern id const NSPasteboardTypeString;
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

#define SEL_(s) sel_registerName(s)
#define CLS(c) ((id)objc_getClass(c))
#define MSG(ret, ...) ((ret(*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

#if defined(__arm64__) || defined(__aarch64__)
#define BOOL_ENC "B"
#else
#define BOOL_ENC "c"
#endif

/* NSEvent types / masks / style constants */
enum {
    EvLeftDown = 1, EvLeftUp = 2, EvRightDown = 3, EvRightUp = 4, EvMouseMoved = 5, EvLeftDragged = 6,
    EvRightDragged = 7, EvKeyDown = 10, EvKeyUp = 11, EvFlagsChanged = 12, EvScrollWheel = 22,
    EvOtherDown = 25, EvOtherUp = 26, EvOtherDragged = 27, EvMagnify = 30
};
enum { FlagShift = 1 << 17, FlagCtrl = 1 << 18, FlagAlt = 1 << 19, FlagCmd = 1 << 20 };

#define MAX_PENDING 64

static struct {
    id app, window, view, glctx;
    CGLContextObj cgl;
    bool headless, quit;
    int headless_w, headless_h;
    int last_w, last_h, last_fbw, last_fbh;
    double mouse_x, mouse_y;
    bool buttons[3];
    uint32_t mods;
    PlatformEvent pending[MAX_PENDING];
    int npending;
    PlatformCursor cursor;
} P;

static id nsstring(const char *s) { return MSG(id, const char *)(CLS("NSString"), SEL_("stringWithUTF8String:"), s); }
static const char *cstring(id s) { return s ? MSG(const char *)(s, SEL_("UTF8String")) : NULL; }

static NSRect msg_rect(id obj, const char *sel) {
#if defined(__x86_64__)
    NSRect r;
    ((void (*)(NSRect *, id, SEL))objc_msgSend_stret)(&r, obj, SEL_(sel));
    return r;
#else
    return MSG(NSRect)(obj, SEL_(sel));
#endif
}

static NSRect msg_rect_rect(id obj, const char *sel, NSRect a) {
#if defined(__x86_64__)
    NSRect r;
    ((void (*)(NSRect *, id, SEL, NSRect))objc_msgSend_stret)(&r, obj, SEL_(sel), a);
    return r;
#else
    return MSG(NSRect, NSRect)(obj, SEL_(sel), a);
#endif
}

static void push_pending(const PlatformEvent *e) {
    if (P.npending < MAX_PENDING) P.pending[P.npending++] = *e;
}

/* ---- runtime-defined classes ---- */

static unsigned long app_should_terminate(id self, SEL cmd, id sender) {
    (void)self, (void)cmd, (void)sender;
    P.quit = true;
    return 0; /* NSTerminateCancel: we shut down cleanly from the main loop */
}

static BOOL window_should_close(id self, SEL cmd, id sender) {
    (void)self, (void)cmd, (void)sender;
    P.quit = true;
    return NO;
}

static BOOL view_yes(id self, SEL cmd) { (void)self, (void)cmd; return YES; }
static BOOL view_yes_arg(id self, SEL cmd, id a) { (void)self, (void)cmd, (void)a; return YES; }
static void view_noop(id self, SEL cmd, id e) { (void)self, (void)cmd, (void)e; }
static unsigned long view_drag_entered(id self, SEL cmd, id info) { (void)self, (void)cmd, (void)info; return 1; }

static BOOL view_perform_drag(id self, SEL cmd, id info) {
    (void)self, (void)cmd;
    id pb = MSG(id)(info, SEL_("draggingPasteboard"));
    id classes = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), CLS("NSURL"));
    id urls = MSG(id, id, id)(pb, SEL_("readObjectsForClasses:options:"), classes, nil);
    unsigned long n = urls ? MSG(unsigned long)(urls, SEL_("count")) : 0;
    NSPoint loc = MSG(NSPoint)(info, SEL_("draggingLocation"));
    NSRect b = msg_rect(P.view, "bounds");
    for (unsigned long i = 0; i < n; i++) {
        id url = MSG(id, unsigned long)(urls, SEL_("objectAtIndex:"), i);
        if (!MSG(BOOL)(url, SEL_("isFileURL"))) continue;
        const char *path = cstring(MSG(id)(url, SEL_("path")));
        if (!path) continue;
        PlatformEvent e = {0};
        e.type = EV_FILE_DROP;
        e.x = loc.x;
        e.y = b.size.height - loc.y;
        str_copy(e.path, sizeof e.path, path);
        push_pending(&e);
    }
    return n > 0;
}

static Class make_class(const char *name, const char *super) {
    Class c = objc_getClass(name);
    if (c) return c;
    return objc_allocateClassPair(objc_getClass(super), name, 0);
}

static void setup_menu(void) {
    id menubar = MSG(id)(MSG(id)(CLS("NSMenu"), SEL_("alloc")), SEL_("init"));
    id app_item = MSG(id)(MSG(id)(CLS("NSMenuItem"), SEL_("alloc")), SEL_("init"));
    MSG(void, id)(menubar, SEL_("addItem:"), app_item);
    MSG(void, id)(P.app, SEL_("setMainMenu:"), menubar);

    id app_menu = MSG(id)(MSG(id)(CLS("NSMenu"), SEL_("alloc")), SEL_("init"));
    id full = MSG(id, id, SEL, id)(MSG(id)(CLS("NSMenuItem"), SEL_("alloc")),
                                   SEL_("initWithTitle:action:keyEquivalent:"), nsstring("Toggle Full Screen"),
                                   SEL_("toggleFullScreen:"), nsstring("f"));
    MSG(void, unsigned long)(full, SEL_("setKeyEquivalentModifierMask:"), (unsigned long)(FlagCmd | FlagCtrl));
    MSG(void, id)(app_menu, SEL_("addItem:"), full);
    id quit = MSG(id, id, SEL, id)(MSG(id)(CLS("NSMenuItem"), SEL_("alloc")),
                                   SEL_("initWithTitle:action:keyEquivalent:"), nsstring("Quit NAVIER"),
                                   SEL_("terminate:"), nsstring("q"));
    MSG(void, id)(app_menu, SEL_("addItem:"), quit);
    MSG(void, id)(app_item, SEL_("setSubmenu:"), app_menu);
}

static bool init_headless(int w, int h) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAccelerated,
        kCGLPFAAllowOfflineRenderers,
        (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix) return false;
    CGLError err = CGLCreateContext(pix, NULL, &P.cgl);
    CGLDestroyPixelFormat(pix);
    if (err != kCGLNoError) return false;
    CGLSetCurrentContext(P.cgl);
    P.headless = true;
    P.headless_w = w;
    P.headless_h = h;
    return true;
}

bool platform_init(const char *title, int width, int height, bool headless) {
    memset(&P, 0, sizeof P);
    if (headless) return init_headless(width, height);

    void *pool = objc_autoreleasePoolPush();
    P.app = MSG(id)(CLS("NSApplication"), SEL_("sharedApplication"));
    MSG(void, long)(P.app, SEL_("setActivationPolicy:"), 0);

    Class del = make_class("NavierAppDelegate", "NSObject");
    class_addMethod(del, SEL_("applicationShouldTerminate:"), (IMP)app_should_terminate, "Q@:@");
    class_addMethod(del, SEL_("windowShouldClose:"), (IMP)window_should_close, BOOL_ENC "@:@");
    objc_registerClassPair(del);
    id delegate = MSG(id)(MSG(id)((id)del, SEL_("alloc")), SEL_("init"));
    MSG(void, id)(P.app, SEL_("setDelegate:"), delegate);

    Class vc = make_class("NavierGLView", "NSView");
    class_addMethod(vc, SEL_("acceptsFirstResponder"), (IMP)view_yes, BOOL_ENC "@:");
    class_addMethod(vc, SEL_("isOpaque"), (IMP)view_yes, BOOL_ENC "@:");
    class_addMethod(vc, SEL_("acceptsFirstMouse:"), (IMP)view_yes_arg, BOOL_ENC "@:@");
    class_addMethod(vc, SEL_("keyDown:"), (IMP)view_noop, "v@:@");
    class_addMethod(vc, SEL_("keyUp:"), (IMP)view_noop, "v@:@");
    class_addMethod(vc, SEL_("flagsChanged:"), (IMP)view_noop, "v@:@");
    class_addMethod(vc, SEL_("draggingEntered:"), (IMP)view_drag_entered, "Q@:@");
    class_addMethod(vc, SEL_("draggingUpdated:"), (IMP)view_drag_entered, "Q@:@");
    class_addMethod(vc, SEL_("prepareForDragOperation:"), (IMP)view_yes_arg, BOOL_ENC "@:@");
    class_addMethod(vc, SEL_("performDragOperation:"), (IMP)view_perform_drag, BOOL_ENC "@:@");
    objc_registerClassPair(vc);

    setup_menu();

    NSRect rect = {{0, 0}, {width, height}};
    unsigned long style = 1 | 2 | 4 | 8 | (1 << 15); /* titled|closable|miniaturizable|resizable|fullSizeContentView */
    P.window = MSG(id, NSRect, unsigned long, unsigned long, BOOL)(
        MSG(id)(CLS("NSWindow"), SEL_("alloc")), SEL_("initWithContentRect:styleMask:backing:defer:"), rect, style,
        2 /* buffered */, NO);
    if (!P.window) return false;
    MSG(void, BOOL)(P.window, SEL_("setReleasedWhenClosed:"), NO);
    MSG(void, id)(P.window, SEL_("setTitle:"), nsstring(title));
    MSG(void, BOOL)(P.window, SEL_("setTitlebarAppearsTransparent:"), YES);
    MSG(void, long)(P.window, SEL_("setTitleVisibility:"), 1);
    MSG(void, id)(P.window, SEL_("setDelegate:"), delegate);
    MSG(void, id)(P.window, SEL_("setBackgroundColor:"), MSG(id)(CLS("NSColor"), SEL_("blackColor")));
    MSG(void, NSSize)(P.window, SEL_("setContentMinSize:"), (NSSize){900, 560});
    id dark = MSG(id, id)(CLS("NSAppearance"), SEL_("appearanceNamed:"), NSAppearanceNameDarkAqua);
    if (dark) MSG(void, id)(P.window, SEL_("setAppearance:"), dark);

    P.view = MSG(id, NSRect)(MSG(id)((id)vc, SEL_("alloc")), SEL_("initWithFrame:"), rect);
    MSG(void, BOOL)(P.view, SEL_("setWantsBestResolutionOpenGLSurface:"), YES);
    id types = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), nsstring("public.file-url"));
    MSG(void, id)(P.view, SEL_("registerForDraggedTypes:"), types);
    MSG(void, id)(P.window, SEL_("setContentView:"), P.view);
    MSG(void, id)(P.window, SEL_("makeFirstResponder:"), P.view);
    MSG(void, BOOL)(P.window, SEL_("setAcceptsMouseMovedEvents:"), YES);
    MSG(void)(P.window, SEL_("center"));

    uint32_t attrs[] = {
        99, 0x4100,  /* NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core */
        8, 24,       /* ColorSize */
        11, 8,       /* AlphaSize */
        12, 24,      /* DepthSize */
        5,           /* DoubleBuffer */
        73,          /* Accelerated */
        0};
    id pf = MSG(id, const uint32_t *)(MSG(id)(CLS("NSOpenGLPixelFormat"), SEL_("alloc")), SEL_("initWithAttributes:"),
                                      attrs);
    if (!pf) {
        LOGE("NSOpenGLPixelFormat: OpenGL 4.1 core profile unavailable");
        return false;
    }
    P.glctx = MSG(id, id, id)(MSG(id)(CLS("NSOpenGLContext"), SEL_("alloc")), SEL_("initWithFormat:shareContext:"), pf,
                              nil);
    if (!P.glctx) return false;

    MSG(void, id)(P.window, SEL_("makeKeyAndOrderFront:"), nil);
    MSG(void, id)(P.glctx, SEL_("setView:"), P.view);
    MSG(void)(P.glctx, SEL_("makeCurrentContext"));
    platform_set_vsync(true);

    MSG(void)(P.app, SEL_("finishLaunching"));
    MSG(void, BOOL)(P.app, SEL_("activateIgnoringOtherApps:"), YES);
    objc_autoreleasePoolPop(pool);

    platform_get_size(&P.last_w, &P.last_h, &P.last_fbw, &P.last_fbh);
    return true;
}

void platform_shutdown(void) {
    if (P.headless) {
        CGLSetCurrentContext(NULL);
        if (P.cgl) CGLDestroyContext(P.cgl);
        P.cgl = NULL;
        return;
    }
    if (P.window) MSG(void)(P.window, SEL_("orderOut:"));
}

void platform_set_vsync(bool on) {
    if (P.headless || !P.glctx) return;
    int32_t v = on ? 1 : 0;
    MSG(void, const int32_t *, long)(P.glctx, SEL_("setValues:forParameter:"), &v, 222);
}

static uint32_t translate_mods(unsigned long f) {
    uint32_t m = 0;
    if (f & FlagShift) m |= MOD_SHIFT;
    if (f & FlagCtrl) m |= MOD_CTRL;
    if (f & FlagAlt) m |= MOD_ALT;
    if (f & FlagCmd) m |= MOD_CMD;
    return m;
}

static int translate_keycode(unsigned short kc) {
    switch (kc) {
    case 53: return KEY_ESCAPE;
    case 36: case 76: return KEY_ENTER;
    case 48: return KEY_TAB;
    case 51: return KEY_BACKSPACE;
    case 117: return KEY_DELETE;
    case 123: return KEY_LEFT;
    case 124: return KEY_RIGHT;
    case 125: return KEY_DOWN;
    case 126: return KEY_UP;
    case 115: return KEY_HOME;
    case 119: return KEY_END;
    case 116: return KEY_PAGEUP;
    case 121: return KEY_PAGEDOWN;
    case 122: return KEY_F1;
    case 120: return KEY_F2;
    case 99: return KEY_F3;
    case 118: return KEY_F4;
    case 96: return KEY_F5;
    case 97: return KEY_F6;
    case 98: return KEY_F7;
    case 100: return KEY_F8;
    case 101: return KEY_F9;
    case 109: return KEY_F10;
    case 103: return KEY_F11;
    case 111: return KEY_F12;
    default: return 0;
    }
}

static uint32_t utf8_next(const unsigned char **ps) {
    const unsigned char *s = *ps;
    uint32_t c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) {
        c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
        s += 3;
    } else if (c >= 0xE0 && s[0] && s[1]) {
        c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u);
        s += 2;
    } else if (c >= 0xC0 && s[0]) {
        c = ((c & 31) << 6) | (s[0] & 63u);
        s += 1;
    }
    *ps = s;
    return c;
}

static void event_location(id ev, double *x, double *y) {
    NSPoint p = MSG(NSPoint)(ev, SEL_("locationInWindow"));
    NSRect b = msg_rect(P.view, "bounds");
    *x = p.x;
    *y = b.size.height - p.y;
}

int platform_poll_events(PlatformEvent *out, int max) {
    int n = 0;
    if (P.headless) return 0;
    void *pool = objc_autoreleasePoolPush();
    id distant = MSG(id)(CLS("NSDate"), SEL_("distantPast"));
    for (;;) {
        id ev = MSG(id, unsigned long, id, id, BOOL)(P.app, SEL_("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                                                     (unsigned long)-1, distant, NSDefaultRunLoopMode, YES);
        if (!ev) break;
        unsigned long type = MSG(unsigned long)(ev, SEL_("type"));
        id ev_window = MSG(id)(ev, SEL_("window"));
        bool ours = ev_window == P.window;
        unsigned long flags = MSG(unsigned long)(ev, SEL_("modifierFlags"));
        PlatformEvent e = {0};
        e.mods = translate_mods(flags);
        bool forward = true;

        switch (type) {
        case EvLeftDown: case EvRightDown: case EvOtherDown:
        case EvLeftUp: case EvRightUp: case EvOtherUp: {
            if (!ours) break;
            bool down = type == EvLeftDown || type == EvRightDown || type == EvOtherDown;
            e.type = down ? EV_MOUSE_DOWN : EV_MOUSE_UP;
            e.button = (type == EvLeftDown || type == EvLeftUp) ? 0 : (type == EvRightDown || type == EvRightUp) ? 1 : 2;
            /* ctrl-click acts as right click on a Mac */
            if (e.button == 0 && (e.mods & MOD_CTRL) && down) e.button = 1;
            e.clicks = (int)MSG(long)(ev, SEL_("clickCount"));
            event_location(ev, &e.x, &e.y);
            if (down) {
                P.buttons[e.button] = true;
            } else {
                /* a ctrl-click "right" press is released as a left-up event */
                if (e.button == 0 && P.buttons[1] && !P.buttons[0]) e.button = 1;
                P.buttons[e.button] = false;
            }
            P.mouse_x = e.x, P.mouse_y = e.y;
            break;
        }
        case EvMouseMoved: case EvLeftDragged: case EvRightDragged: case EvOtherDragged:
            if (!ours) break;
            e.type = EV_MOUSE_MOVE;
            event_location(ev, &e.x, &e.y);
            e.dx = e.x - P.mouse_x;
            e.dy = e.y - P.mouse_y;
            P.mouse_x = e.x, P.mouse_y = e.y;
            break;
        case EvScrollWheel:
            if (!ours) break;
            e.type = EV_SCROLL;
            e.dx = MSG(double)(ev, SEL_("scrollingDeltaX"));
            e.dy = MSG(double)(ev, SEL_("scrollingDeltaY"));
            e.precise = MSG(BOOL)(ev, SEL_("hasPreciseScrollingDeltas"));
            if (!e.precise) e.dx *= 10.0, e.dy *= 10.0;
            event_location(ev, &e.x, &e.y);
            break;
        case EvMagnify:
            if (!ours) break;
            e.type = EV_MAGNIFY;
            e.magnify = MSG(double)(ev, SEL_("magnification"));
            event_location(ev, &e.x, &e.y);
            break;
        case EvKeyDown: case EvKeyUp: {
            unsigned short kc = MSG(unsigned short)(ev, SEL_("keyCode"));
            int key = translate_keycode(kc);
            const char *ign = cstring(MSG(id)(ev, SEL_("charactersIgnoringModifiers")));
            if (!key && ign && ign[0]) {
                const unsigned char *s = (const unsigned char *)ign;
                uint32_t cp = utf8_next(&s);
                if (cp < 128) key = (cp >= 'A' && cp <= 'Z') ? (int)(cp - 'A' + 'a') : (int)cp;
            }
            e.type = type == EvKeyDown ? EV_KEY_DOWN : EV_KEY_UP;
            e.key = key;
            e.repeat = MSG(BOOL)(ev, SEL_("isARepeat"));
            if (n < max) out[n++] = e;
            if (type == EvKeyDown && !(e.mods & (MOD_CMD | MOD_CTRL))) {
                const char *chars = cstring(MSG(id)(ev, SEL_("characters")));
                if (chars) {
                    const unsigned char *s = (const unsigned char *)chars;
                    while (*s) {
                        uint32_t cp = utf8_next(&s);
                        if (cp < 32 || cp == 127 || (cp >= 0xF700 && cp <= 0xF8FF)) continue;
                        PlatformEvent t = {0};
                        t.type = EV_TEXT;
                        t.codepoint = cp;
                        t.mods = e.mods;
                        if (n < max) out[n++] = t;
                    }
                }
            }
            e.type = EV_NONE;
            /* Let Cmd-shortcuts reach the menu; everything else is ours. */
            forward = (e.mods & MOD_CMD) != 0;
            break;
        }
        case EvFlagsChanged:
            P.mods = e.mods;
            break;
        default:
            break;
        }
        P.mods = e.mods;
        if (e.type != EV_NONE && n < max) out[n++] = e;
        if (forward) MSG(void, id)(P.app, SEL_("sendEvent:"), ev);
    }
    MSG(void)(P.app, SEL_("updateWindows"));

    for (int i = 0; i < P.npending && n < max; i++) out[n++] = P.pending[i];
    P.npending = 0;

    int w, h, fbw, fbh;
    platform_get_size(&w, &h, &fbw, &fbh);
    if (w != P.last_w || h != P.last_h || fbw != P.last_fbw || fbh != P.last_fbh) {
        MSG(void)(P.glctx, SEL_("update"));
        P.last_w = w, P.last_h = h, P.last_fbw = fbw, P.last_fbh = fbh;
        if (n < max) {
            PlatformEvent r = {0};
            r.type = EV_RESIZE;
            out[n++] = r;
        }
    }
    objc_autoreleasePoolPop(pool);
    return n;
}

bool platform_should_quit(void) { return P.quit; }
void platform_request_quit(void) { P.quit = true; }

void platform_swap(void) {
    if (P.headless) {
        glFlush();
        return;
    }
    MSG(void)(P.glctx, SEL_("flushBuffer"));
}

void platform_get_size(int *w, int *h, int *fbw, int *fbh) {
    if (P.headless) {
        if (w) *w = P.headless_w;
        if (h) *h = P.headless_h;
        if (fbw) *fbw = P.headless_w;
        if (fbh) *fbh = P.headless_h;
        return;
    }
    NSRect b = msg_rect(P.view, "bounds");
    NSRect px = msg_rect_rect(P.view, "convertRectToBacking:", b);
    if (w) *w = (int)b.size.width;
    if (h) *h = (int)b.size.height;
    if (fbw) *fbw = (int)px.size.width;
    if (fbh) *fbh = (int)px.size.height;
}

double platform_backing_scale(void) {
    if (P.headless) return 1.0;
    return MSG(double)(P.window, SEL_("backingScaleFactor"));
}

void platform_get_mouse(double *x, double *y) {
    *x = P.mouse_x;
    *y = P.mouse_y;
}

bool platform_mouse_down(int button) { return button >= 0 && button < 3 && P.buttons[button]; }
uint32_t platform_mods(void) { return P.mods; }

bool platform_window_focused(void) {
    if (P.headless) return true;
    return MSG(BOOL)(P.window, SEL_("isKeyWindow"));
}

void platform_set_title(const char *title) {
    if (P.headless) return;
    void *pool = objc_autoreleasePoolPush();
    MSG(void, id)(P.window, SEL_("setTitle:"), nsstring(title));
    objc_autoreleasePoolPop(pool);
}

void platform_set_cursor(PlatformCursor c) {
    if (P.headless || c == P.cursor) return;
    P.cursor = c;
    static const char *names[] = {"arrowCursor", "IBeamCursor", "pointingHandCursor", "crosshairCursor",
                                  "openHandCursor", "closedHandCursor", "resizeLeftRightCursor"};
    MSG(void)(MSG(id)(CLS("NSCursor"), SEL_(names[c])), SEL_("set"));
}

bool platform_open_file_dialog(char *out, size_t cap, const char *ext) {
    if (P.headless) return false;
    bool ok = false;
    void *pool = objc_autoreleasePoolPush();
    id panel = MSG(id)(CLS("NSOpenPanel"), SEL_("openPanel"));
    MSG(void, BOOL)(panel, SEL_("setCanChooseFiles:"), YES);
    MSG(void, BOOL)(panel, SEL_("setCanChooseDirectories:"), NO);
    MSG(void, BOOL)(panel, SEL_("setAllowsMultipleSelection:"), NO);
    if (ext) {
        id arr = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), nsstring(ext));
        MSG(void, id)(panel, SEL_("setAllowedFileTypes:"), arr);
    }
    if (MSG(long)(panel, SEL_("runModal")) == 1) {
        id urls = MSG(id)(panel, SEL_("URLs"));
        if (MSG(unsigned long)(urls, SEL_("count")) > 0) {
            id url = MSG(id, unsigned long)(urls, SEL_("objectAtIndex:"), 0);
            const char *p = cstring(MSG(id)(url, SEL_("path")));
            if (p) str_copy(out, cap, p), ok = true;
        }
    }
    /* the modal loop consumed the mouse-up of the click that opened it */
    P.buttons[0] = P.buttons[1] = P.buttons[2] = false;
    MSG(void, id)(P.window, SEL_("makeKeyAndOrderFront:"), nil);
    MSG(void)(P.glctx, SEL_("makeCurrentContext"));
    objc_autoreleasePoolPop(pool);
    return ok;
}

bool platform_save_file_dialog(char *out, size_t cap, const char *default_name) {
    if (P.headless) return false;
    bool ok = false;
    void *pool = objc_autoreleasePoolPush();
    id panel = MSG(id)(CLS("NSSavePanel"), SEL_("savePanel"));
    if (default_name) MSG(void, id)(panel, SEL_("setNameFieldStringValue:"), nsstring(default_name));
    if (MSG(long)(panel, SEL_("runModal")) == 1) {
        const char *p = cstring(MSG(id)(MSG(id)(panel, SEL_("URL")), SEL_("path")));
        if (p) str_copy(out, cap, p), ok = true;
    }
    P.buttons[0] = P.buttons[1] = P.buttons[2] = false;
    MSG(void, id)(P.window, SEL_("makeKeyAndOrderFront:"), nil);
    MSG(void)(P.glctx, SEL_("makeCurrentContext"));
    objc_autoreleasePoolPop(pool);
    return ok;
}

bool platform_clipboard_get(char *out, size_t cap) {
    if (P.headless) return false;
    bool ok = false;
    void *pool = objc_autoreleasePoolPush();
    id pb = MSG(id)(CLS("NSPasteboard"), SEL_("generalPasteboard"));
    id s = MSG(id, id)(pb, SEL_("stringForType:"), NSPasteboardTypeString);
    const char *c = cstring(s);
    if (c && c[0]) str_copy(out, cap, c), ok = true;
    objc_autoreleasePoolPop(pool);
    return ok;
}

void platform_clipboard_set(const char *text) {
    if (P.headless) return;
    void *pool = objc_autoreleasePoolPush();
    id pb = MSG(id)(CLS("NSPasteboard"), SEL_("generalPasteboard"));
    MSG(long)(pb, SEL_("clearContents"));
    MSG(BOOL, id, id)(pb, SEL_("setString:forType:"), nsstring(text), NSPasteboardTypeString);
    objc_autoreleasePoolPop(pool);
}

void platform_open_path(const char *path) {
    if (P.headless) return;
    void *pool = objc_autoreleasePoolPush();
    id ws = MSG(id)(CLS("NSWorkspace"), SEL_("sharedWorkspace"));
    id url = MSG(id, id)(CLS("NSURL"), SEL_("fileURLWithPath:"), nsstring(path));
    id arr = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), url);
    MSG(void, id)(ws, SEL_("activateFileViewerSelectingURLs:"), arr);
    objc_autoreleasePoolPop(pool);
}
