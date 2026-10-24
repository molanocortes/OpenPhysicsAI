/* xrlink.h - the headset link's shared rules (docs/xr.md): where its folder is, and which view commands the headset,
 * or the AI through openphysics-mcp's headset tools, may give the app. The app (src/xr.c) runs a command only when this
 * rule allows it; the bridge (tools/xr/xr_bridge.py) applies the same rule, and tools/xr/commands.json holds the lines
 * both are tested on. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* $OPENPHYSICS_XR_DIR (or $NAVIER_XR_DIR), else ~/.openphysicsai/run/xr (or ~/.navier/run/xr while only that exists);
 * false when there is no home to put it in */
bool xrlink_dir(char *out, size_t cap);
/* playback, a frame or a stored time, a field, a section, the deformation scale: "lab ..." or "fem ..." only, printable
 * ASCII, at most 120 bytes; never a file, a run, or any other terminal command */
bool xrlink_command_allowed(const char *line);
