/* xr.h - the headset link: the result on screen, seen as a 3D object in a Meta Quest (docs/xr.md).
 *
 * While tools/xr/xr_bridge.py runs it keeps a folder (~/.openphysicsai/run/xr, or $OPENPHYSICS_XR_DIR). The app writes into it:
 *   scene.glb   the finite-element result it is showing (glTF binary: the deformed surface in metres, Y up, resting on
 *               its lowest point, each vertex its value and its place in the colour map, the way back to the part's
 *               build coordinates in mm in the extras);
 *   state.json  what is shown: the finite-element field, range and stored times, or the lab result's file, frame and
 *               view (src/lab/labxr.h, which the bridge has build/labxr draw frame by frame), and what the AI is doing,
 *               a permission prompt waiting, and its last answer.
 * The bridge writes back ask-*.txt (a question, typed or spoken in the headset), cmd-*.txt (a view command: playback,
 * a frame, a field, a section; xrlink_command_allowed) and answer.txt ("allow" or "deny"). Without the folder nothing is
 * written. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void xr_tick(void);                         /* once a frame: cheap until the bridge's folder exists */
bool xr_export_glb(const char *path, char *err, size_t cap); /* the result on screen as a GLB (false: nothing to export) */
/* the view commands the headset may give are src/core/xrlink.h's (xrlink_command_allowed), shared with openphysics-mcp */
