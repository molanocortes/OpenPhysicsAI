/* voice.h - the microphone of the agentic bar (GOALS.md G18)
 *
 * A press starts listening, a second press stops; the words come back as text from the Mac's own speech recognition
 * (on the device when the language allows), through the helper build/openphysics-mic (tools/voice/mic.swift) run as a child
 * process. OPENPHYSICS_VOICE_HELPER names another program to run instead (a test's stand-in); OPENPHYSICS_VOICE_LANG the language,
 * for example en-US or de-DE (default: the Mac's first preferred language). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

bool voice_start(char *err, size_t errlen); /* false with a reason (no helper, already listening, cannot start) */
void voice_stop(void);                      /* stop listening: the helper then sends the words */
bool voice_listening(void);                 /* started and not yet finished */
/* once per frame: 1 with the words when the helper has finished, -1 with a reason when it failed, 0 otherwise */
int voice_poll(char *words, size_t cap, char *err, size_t errlen);
