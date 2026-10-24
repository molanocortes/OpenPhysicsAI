/* voice.c - the microphone of the agentic bar (see voice.h) */
#include "voice.h"
#include "core/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pid_t g_pid;
static int g_out = -1, g_err = -1;
static char g_words[2048], g_why[512];
static size_t g_nw, g_ny;

/* the helper: the stand-in a test names, else openphysics-mic beside the executable (the app bundle), else build/ */
static bool helper_path(char *out, size_t cap) {
    const char *env = op_getenv("VOICE_HELPER");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return true;
    }
    char exe[1024];
    uint32_t sz = sizeof exe;
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, cap, "%s/openphysics-mic", exe);
            if (access(out, X_OK) == 0) return true;
            snprintf(out, cap, "%s/build/openphysics-mic", exe);
            if (access(out, X_OK) == 0) return true;
        }
    }
    snprintf(out, cap, "build/openphysics-mic");
    return access(out, X_OK) == 0;
}

bool voice_listening(void) { return g_pid > 0; }

bool voice_start(char *err, size_t errlen) {
    if (g_pid > 0) {
        snprintf(err, errlen, "already listening");
        return false;
    }
    char path[1100];
    if (!helper_path(path, sizeof path)) {
        snprintf(err, errlen, "no voice helper: build it with make voice");
        return false;
    }
    int out[2], er[2];
    if (pipe(out) != 0) {
        snprintf(err, errlen, "cannot listen: %s", strerror(errno));
        return false;
    }
    if (pipe(er) != 0) {
        close(out[0]), close(out[1]);
        snprintf(err, errlen, "cannot listen: %s", strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawn_file_actions_addclose(&fa, er[0]);
    const char *lang = op_getenv("VOICE_LANG");
    char *argv[] = {path, (char *)(lang && *lang ? lang : NULL), NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]), close(er[1]);
    if (rc != 0) {
        close(out[0]), close(er[0]);
        snprintf(err, errlen, "the voice helper could not start: %s", strerror(rc));
        return false;
    }
    fcntl(out[0], F_SETFL, O_NONBLOCK), fcntl(er[0], F_SETFL, O_NONBLOCK);
    g_pid = pid, g_out = out[0], g_err = er[0], g_nw = g_ny = 0, g_words[0] = g_why[0] = 0;
    return true;
}

void voice_stop(void) {
    if (g_pid > 0) kill(g_pid, SIGINT);
}

static void drain(int fd, char *buf, size_t cap, size_t *n) {
    if (fd < 0) return;
    for (;;) {
        char tmp[512];
        ssize_t k = read(fd, tmp, sizeof tmp);
        if (k <= 0) return;
        size_t room = cap - 1 - *n, take = (size_t)k < room ? (size_t)k : room;
        memcpy(buf + *n, tmp, take);
        *n += take;
        buf[*n] = 0;
    }
}

int voice_poll(char *words, size_t cap, char *err, size_t errlen) {
    if (g_pid <= 0) return 0;
    drain(g_out, g_words, sizeof g_words, &g_nw);
    drain(g_err, g_why, sizeof g_why, &g_ny);
    int status = 0;
    pid_t w = waitpid(g_pid, &status, WNOHANG);
    if (w == 0) return 0;
    drain(g_out, g_words, sizeof g_words, &g_nw);
    drain(g_err, g_why, sizeof g_why, &g_ny);
    close(g_out), close(g_err);
    g_out = g_err = -1, g_pid = 0;
    while (g_nw && (g_words[g_nw - 1] == '\n' || g_words[g_nw - 1] == '\r' || g_words[g_nw - 1] == ' ')) g_words[--g_nw] = 0;
    while (g_ny && (g_why[g_ny - 1] == '\n' || g_why[g_ny - 1] == '\r')) g_why[--g_ny] = 0;
    if (w > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && g_nw) {
        snprintf(words, cap, "%s", g_words);
        return 1;
    }
    snprintf(err, errlen, "%s", g_ny ? g_why : "no words were heard");
    return -1;
}
