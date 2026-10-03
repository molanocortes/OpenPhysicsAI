/* labapp.h - the physics lab inside the app: any lab result (src/lab/labio.h) opened, played, turned and zoomed in the
 * window. Native captures use this renderer; portable labfilm uses the software reference in src/lab/labview.h.
 *
 * Terminal: lab open FILE.lab | lab run SCENARIO.json | lab close | lab field NAME | lab frame N | lab play | lab pause |
 *           lab next | lab prev | lab fps N | lab view iso|top|front|... | lab orbit AZIM ELEV | lab mesh on|off |
 *           lab mirror on|off | lab meshbelow on|off | lab cmap NAME | lab range LO HI | lab range auto | lab info
 * Mouse: drag turns a 3D result, the wheel zooms. */
#pragma once

#include <stdbool.h>

bool labapp_active(void);
bool labapp_open(const char *path);
void labapp_close(void);
void labapp_command(int argc, char **argv);
void labapp_tick(double dt);
/* draws the current frame into the target framebuffer, in the rectangle x0..x1, y0..y1 (framebuffer pixels, y up):
 * the viewport beside the panel, below the toolbar and above the hint line */
void labapp_draw(unsigned int target_fbo, int x0, int x1, int y0, int y1);
void labapp_orbit(float dx, float dy);
void labapp_zoom(float factor);
/* the open result's title */
const char *labapp_title(void);
/* one line for the status bar: the result, the frame and the time */
const char *labapp_status(void);

/* ---- for the panels (hud.c): what is open, and the few things a person changes by hand ---- */
int labapp_frame(void);
int labapp_nframes(void);
double labapp_frame_time(int frame);
void labapp_set_frame(int frame);
bool labapp_playing(void);
void labapp_set_playing(bool on);
const char *labapp_field(void);
void labapp_set_field(const char *name);
int labapp_field_names(char names[][48], int max); /* the fields the result carries (not solid or material) */
const char *labapp_domain(void);
const char *labapp_time_unit(void); /* "" for seconds; a domain's own unit otherwise (relativity: M) */
bool labapp_running(void);           /* a scenario is being run in the background */
const char *labapp_run_name(void);   /* the scenario being run */
const char *labapp_run_error(void);  /* a failed run remains visible in the manual panel */
void labapp_run(const char *scenario_path);
void labapp_select(const char *scenario_path); /* reopen its saved result or run it on first use */
double labapp_fps(void);
/* a scenario's own knobs: "controls": [{"label", "path" (dots, array indices as numbers), "min", "max", "log"}] in the
 * scenario the open result was run from; changing one and rerunning makes a new result */
typedef struct LabControl {
    char label[64];
    double value, min, max;
    bool log;
} LabControl;
int labapp_controls(LabControl *out, int max);
void labapp_control_set(int i, double value);
void labapp_rerun(void);

/* Agent mode: the window follows the lab's results folder (~/NAVIER-Projects/lab), opening a new result as soon as it
 * has finished being written, the way it follows an agent's analysis jobs */
void labapp_follow(double dt);
const char *labapp_results_dir(void);
/* the most recent results in that folder, newest first: file paths and titles */
int labapp_recent(char paths[][1024], char titles[][120], int max);

/* the legend the panel draws: range, the palette as the picture uses it, the field and its unit */
bool labapp_legend(double *lo, double *hi, int *cmap, const char **field, const char **unit);

bool labapp_native3d(void);
int labapp_section_axis(void);
bool labapp_section_flipped(void);
double labapp_section_fraction(void);
void labapp_section_set(int axis,double fraction);

bool labapp_is_water(void);
bool labapp_water_surface(void);
