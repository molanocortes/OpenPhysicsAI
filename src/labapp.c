#include <time.h>
#include <dirent.h>
/* labapp.c - see labapp.h. Points and unstructured cells retain one frame on the GPU.
 * Other results use the labview software reference image and framebuffer blit. */
#include "labapp.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>

#include "app.h"
#include "common.h"
#include "core/json.h"
#include "glutil.h"
#include "labgpu.h"
#include "lab/labwater.h"
#include "lab/lab_domains.h"
#include "lab/labio.h"
#include "lab/labview.h"
#include "lab/style.h"

typedef struct LabApp {
    bool active;
    LabFile *lf;
    char path[1024];
    int frame, nframes;
    bool playing;
    double fps, acc;
    LabViewOpts opt;
    char field[48], title[200], unit[32], view[16];
    double lo, hi;
    bool auto_range;
    bool dirty, rough;
    double idle;               /* seconds since the view last moved */
    GLuint tex, fbo;
    LabGpu *gpu;
    LabFrame cached;
    int cached_frame, uploaded_frame, volume_frame, section_axis;
    char volume_field[48];
    char iso_field[48];
    double iso_level;
    double section_fraction;
    bool section_flip, geometry_dirty, use_gpu, native3d, water_surface;
    double water_spacing;
    unsigned reads, uploads, bench_left, bench_count;
    bool bench_frames;
    double bench_ms;
    unsigned bench_reads, bench_uploads;
    int tw, th;
    char status[256];
    /* a scenario run in the background */
    pthread_t worker;
    bool running, run_ok;
    _Atomic bool finished; /* worker publishes run_ok and run_err to the UI thread */
    char run_out[1024], run_err[512];
    JsonValue *run_scenario;
    char domain[32], time_unit[16];
    JsonValue *scenario;       /* the scenario the open result came from (run from the app), with its controls */
    char scenario_path[1024], run_name[128];
} LabApp;

static LabApp g;

bool labapp_active(void) { return g.active; }

/* the header's unit of a field */
static void field_unit(const char *field, char *out, size_t n) {
    out[0] = 0;
    JsonValue *h = json_parse(lab_header(g.lf), strlen(lab_header(g.lf)), NULL, NULL);
    const char *u = h ? json_get_str(json_get(h, "fields"), field, "") : "";
    snprintf(out, n, "%s", u);
    json_free(h);
}

static void set_field(const char *name) {
    if (name != g.field) snprintf(g.field, sizeof g.field, "%s", name);
    g.opt.field = g.field;
    field_unit(name, g.unit, sizeof g.unit);
    g.opt.unit = g.unit;
    if (g.auto_range) {
        LabViewOpts o = g.opt;
        o.first = 0, o.last = g.nframes - 1, o.every = g.nframes > 40 ? g.nframes / 20 : 1;
        labview_range(g.lf, &o, &g.lo, &g.hi);
    }
    g.geometry_dirty = true;
    g.dirty = true;
}

/* A result's input snapshot carries the controls that produced it. Never keep the previous result's controls. */
static void restore_scenario(const char *path) {
    json_free(g.scenario);
    g.scenario = NULL;
    g.scenario_path[0] = 0;
    char source[1100];
    snprintf(source, sizeof source, "%s", path);
    char *ext = strrchr(source, '.');
    if (ext) snprintf(ext, sizeof source - (size_t)(ext - source), ".json");
    g.scenario = json_read_file(source, 16u << 20, NULL);
    if (!g.scenario) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char resource[256];
        snprintf(resource, sizeof resource, "examples/lab/%.*s.json", (int)strcspn(base, "."), base);
        if (app_resource_path(resource, source, sizeof source)) g.scenario = json_read_file(source, 16u << 20, NULL);
    }
    if (g.scenario) snprintf(g.scenario_path, sizeof g.scenario_path, "%s", source);
}

bool labapp_open(const char *path) {
    char err[256];
    LabFile *lf = lab_open(path, err, sizeof err);
    if (!lf) {
        LOGE("lab: %s", err);
        return false;
    }
    if (lab_frame_count(lf) < 1) {
        LOGE("lab: %s holds no frame", path);
        lab_close_file(lf);
        return false;
    }
    labapp_close();
    restore_scenario(path);
    g.lf = lf;
    snprintf(g.path, sizeof g.path, "%s", path);
    g.cached_frame = g.uploaded_frame = g.volume_frame = -1;g.volume_field[0]=0;
    g.section_axis = -1; g.section_fraction = 0.5; g.section_flip = false;
    g.geometry_dirty = true; g.use_gpu = true; g.native3d = false;g.water_surface=false;g.water_spacing=0;
    g.reads = g.uploads = g.bench_left = 0;
    g.nframes = lab_frame_count(lf);
    g.frame = 0;
    g.fps = 12;
    g.auto_range = true;
    labview_defaults(&g.opt);
    g.opt.supersample = 1;
    g.opt.no_chrome = true; /* the panel shows the title and the time */
    g.opt.no_colorbar = true; /* and the legend, crisp at any size */
    /* the title from the header, the first field of the first part as the one shown */
    JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL);
    snprintf(g.title, sizeof g.title, "%s", h ? json_get_str(h, "title", "") : "");
    const char *domain = h ? json_get_str(h, "domain", "") : "";
    snprintf(g.domain, sizeof g.domain, "%s", domain);
    snprintf(g.time_unit, sizeof g.time_unit, "%s", h ? json_get_str(h, "time_unit", "") : ""); /* relativity: M */
    g.opt.title = g.title;
    if (!strcmp(domain,"impact")) g.opt.solid_points = true;
    if (strcmp(domain, "water") && h && json_get_num(h, "particle_spacing_m", 0) > 0) /* particles of a solid, sized from their spacing */
        g.opt.point_radius_m = 0.55 * json_get_num(h, "particle_spacing_m", 0);
    if (!strcmp(domain,"water")) {
        g.water_spacing=json_get_num(h,"particle_spacing_m",0);g.opt.point_radius_m=.62*g.water_spacing;
        g.water_surface=g.water_spacing>0;
    }
    /* sensible first looks per domain: an axisymmetric gas run is mirrored with its mesh below, points and meshes are 3D */
    if (!strcmp(domain, "gas") && h && strstr(json_get_str(h, "geometry", ""), "axisymmetric")) g.opt.mesh_below = g.opt.mirror = true;
    /* one world: every domain in the lab's own palette and room (src/lab/style.h); only what a domain needs to be read */
    if (!strcmp(domain, "impact")) g.opt.mesh = true;
    if (!strcmp(domain, "acoustic")) g.opt.walls = true;
    g.opt.solid_volume = h && !strcmp(json_get_str(h, "volume_look", ""), "solid");
    if (h && !strcmp(json_get_str(h, "palette", ""), "glass")) g.opt.cmap = 103; /* a result may ask for its palette */
    if (h && !strcmp(json_get_str(h, "palette", ""), "fire")) g.opt.cmap = 104;
    /* "volume_look": "iso": surfaces where iso_field crosses iso_level (vortex cores), coloured by the shown field */
    g.iso_field[0] = 0, g.iso_level = 0;
    if (h && !strcmp(json_get_str(h, "volume_look", ""), "iso"))
        snprintf(g.iso_field, sizeof g.iso_field, "%s", json_get_str(h, "iso_field", "")), g.iso_level = json_get_num(h, "iso_level", 0);
    g.opt.nlooks = 0; /* parts drawn as materials: "looks": {"windings": "copper", ...} */
    const JsonValue *looks = h ? json_get(h, "looks") : NULL;
    for (size_t i = 0; looks && i < json_len(looks) && g.opt.nlooks < 12; i++) {
        const char *key = json_key_at(looks, i), *val = json_str(json_value_at(looks, i));
        int id = labscene_look_id(val);
        if (key && id) snprintf(g.opt.looks[g.opt.nlooks].part, sizeof g.opt.looks[0].part, "%s", key), g.opt.looks[g.opt.nlooks++].look = id;
    }
    if (!strcmp(domain, "orbit")) g.opt.labels = true, g.opt.true_size = true, g.opt.radius = 3;
    json_free(h);
    LabFrame fr;
    const char *first = "";
    static char firstbuf[48];
    if (lab_read_frame(lf, 0, &fr, err, sizeof err)) {
        g.native3d = (labscene_supported(&fr) || labscene_volume(&fr)) && strcmp(g.domain,"orbit");
        /* the first field that varies (a uniform one shows nothing); failing that, the first field */
        LabFrame lastf;
        bool have_last = lab_read_frame(lf, g.nframes - 1, &lastf, err, sizeof err);
        const LabFrame *scan = have_last ? &lastf : &fr;
        for (int pass = 0; pass < 2 && !first[0]; pass++)
            for (int p = 0; p < scan->nparts && !first[0]; p++)
                for (int k = 0; k < scan->parts[p].nfields && !first[0]; k++) {
                    const LabField *F = &scan->parts[p].fields[k];
                    if (!strcmp(F->name, "solid") || !strcmp(F->name, "material") || !strcmp(F->name, "body")) continue;
                    double lo = 1e300, hi = -1e300; /* finite sentinels: the app builds with fast-math */
                    for (size_t q = 0; q < F->count; q++)
                        if (F->data[q] > -1e30f && F->data[q] < 1e30f) lo = fmin(lo, F->data[q]), hi = fmax(hi, F->data[q]);
                    bool varies = hi > lo && (hi - lo) > 0.01 * fmax(fabs(hi), fabs(lo));
                    if (pass == 1 || varies) {
                        snprintf(firstbuf, sizeof firstbuf, "%s", F->name);
                        first = firstbuf;
                    }
                }
        if (have_last) lab_frame_free(&lastf);
        for (int p = 0; p < fr.nparts; p++)
            if (lab_find_field(&fr.parts[p], "solid")) g.opt.solid = "solid";
            else if (lab_find_field(&fr.parts[p], "material") && fr.parts[p].kind == LAB_BLOCKS) g.opt.solid = "material";
        lab_frame_free(&fr);
    }
    set_field(first);
    g.active = true;
    g.playing = g.nframes > 1;
    g.dirty = true;
    if (app.status.running) sim_run(app.sim, false);
    LOGOK("lab: %s, %d frames (drag to turn a 3D result, the wheel zooms; 'lab help')", path, g.nframes);
    return true;
}

void labapp_close(void) {
    lab_frame_free(&g.cached); g.cached_frame = -1;
    labgpu_destroy(g.gpu); g.gpu = NULL;
    if (g.lf) lab_close_file(g.lf);
    g.lf = NULL;
    g.active = false;
    g.playing = false;
}

static void *run_worker(void *arg) {
    LabRunInfo info;
    g.run_ok = lab_run_domain(json_get_str(g.run_scenario, "domain", ""), g.run_scenario, g.run_out, true, &info, g.run_err, sizeof g.run_err);
    g.finished = true;
    return NULL;
}

static void start_run(const char *scenario) {
    if (g.running) {
        LOGW("lab: a run is already going");
        return;
    }
    JsonError je;
    JsonValue *root = json_read_file(scenario, 16u << 20, &je);
    if (!root) {
        LOGE("lab: %s: line %d: %s", scenario, je.line, je.message);
        return;
    }
    const char *home = getenv("HOME");
    char dir[900];
    snprintf(dir, sizeof dir, "%s/NAVIER-Projects", home ? home : "/tmp");
    mkdir(dir, 0755); /* both levels: on a first start neither exists */
    snprintf(dir, sizeof dir, "%s/NAVIER-Projects/lab", home ? home : "/tmp");
    mkdir(dir, 0755);
    const char *base = strrchr(scenario, '/');
    base = base ? base + 1 : scenario;
    snprintf(g.run_out, sizeof g.run_out, "%s/%.*s.lab", dir, (int)(strcspn(base, ".")), base);
    char snapshot[1100];
    snprintf(snapshot, sizeof snapshot, "%s/%.*s.json", dir, (int)strcspn(base, "."), base);
    if (!json_write_file(snapshot, root, JSON_PRETTY)) {
        json_free(root);
        snprintf(g.run_err, sizeof g.run_err, "Cannot save the scenario input snapshot.");
        LOGE("lab: %s", g.run_err);
        return;
    }
    g.run_scenario = root;
    json_free(g.scenario);
    g.scenario = json_clone(root);
    snprintf(g.scenario_path, sizeof g.scenario_path, "%s", scenario);
    snprintf(g.run_name, sizeof g.run_name, "%s", json_get_str(root, "title", base));
    g.finished = false;
    g.run_err[0] = 0;
    g.running = true;
    if (pthread_create(&g.worker, NULL, run_worker, NULL) != 0) {
        g.running = false;
        json_free(root);
        LOGE("lab: cannot start the run");
        return;
    }
    LOGI("lab: running %s in the background, into %s", scenario, g.run_out);
}

void labapp_tick(double dt) {
    if (g.running && g.finished) {
        pthread_join(g.worker, NULL);
        g.running = false;
        json_free(g.run_scenario);
        g.run_scenario = NULL;
        if (g.run_ok) labapp_open(g.run_out);
        else LOGE("lab: the run failed: %s", g.run_err);
    }
    if (!g.active) return;
    g.idle += dt;
    if (g.rough && g.idle > 0.25) g.rough = false, g.dirty = true; /* the view stopped: sharp again */
    if (g.playing && g.nframes > 1 && !g.bench_left) {
        g.acc += dt;
        if (g.acc >= 1.0 / g.fps) {
            int advance=(int)(g.acc*g.fps);
            g.acc=fmod(g.acc,1.0/g.fps);
            g.frame = (g.frame + advance) % g.nframes;
            g.dirty = true;
            g.rough = true, g.idle = 0; /* playing: half size, so the frames keep coming; sharp again on pause */
        }
    }
}

void labapp_orbit(float dx, float dy) {
    if (!g.active) return;
    if (!g.opt.angles) g.opt.angles = true, g.opt.azim = -55, g.opt.elev = 25;
    g.opt.azim -= dx * 0.4;
    g.opt.elev += dy * 0.4;
    if (g.opt.elev > 89) g.opt.elev = 89;
    if (g.opt.elev < -89) g.opt.elev = -89;
    g.rough = true, g.idle = 0, g.dirty = true;
}

void labapp_zoom(float f) {
    if (!g.active) return;
    g.opt.zoom *= f;
    if (g.opt.zoom < 0.2) g.opt.zoom = 0.2;
    if (g.opt.zoom > 200) g.opt.zoom = 200;
    g.rough = true, g.idle = 0, g.dirty = true;
}

const char *labapp_title(void) { return g.active ? g.title : ""; }

const char *labapp_status(void) {
    if (!g.active) return "";
    snprintf(g.status, sizeof g.status, "LAB  %s  frame %d/%d  t = %.4g %s  %s", g.title, g.frame + 1, g.nframes, lab_frame_time(g.lf, g.frame),
             g.time_unit[0] ? g.time_unit : "s",
             g.playing ? "playing" : "paused");
    return g.status;
}

void labapp_draw(unsigned int target, int x0, int x1, int y0, int y1) {
    if (!g.active) return;
    int w = x1 - x0, h = y1 - y0;
    if (w < 64 || h < 64) return;
    double draw_start=now_seconds();
    if(g.bench_left && g.bench_frames)g.frame=(g.frame+1)%g.nframes;
    if (g.native3d && g.use_gpu) {
        char err[128];
        if (g.cached_frame != g.frame) {
            lab_frame_free(&g.cached);
            if (!lab_read_frame(g.lf,g.frame,&g.cached,err,sizeof err)) { LOGE("lab: %s",err);return; }
            g.cached_frame=g.frame;g.reads++;g.geometry_dirty=true;
        }
        if (!g.gpu) g.gpu=labgpu_create();
        if (!g.gpu) { LOGE("lab: GPU renderer unavailable; using software");g.use_gpu=false;g.dirty=true;return; }
        if (g.geometry_dirty || g.uploaded_frame!=g.frame) {
            const LabPart *volume=labscene_volume(&g.cached);bool ok=true,uploaded=false;
            bool texture_current=g.volume_frame==g.frame && !strcmp(g.volume_field,g.field);
            /* the geometry (bodies, particles, lines) and at most one computed volume, drawn together in one scene */
            LabScene scene;
            if (!labscene_build(&g.cached,&g.opt,g.section_axis,g.section_fraction,g.section_flip,&scene)) { LOGE("lab: cannot build 3D geometry");return; }
            ok=labgpu_upload(g.gpu,&scene);uploaded=scene.ntri+scene.npoints+scene.nlines>0;labscene_free(&scene);
            if(!volume && !g.water_surface)labgpu_clear_volume(g.gpu);
            if(ok && volume && !texture_current){
                ok=g.iso_field[0]?labgpu_upload_volume_iso(g.gpu,volume,g.field,g.iso_field,g.iso_level):labgpu_upload_volume(g.gpu,volume,g.field);
                uploaded=true;
            }
            else if(ok && !volume && g.water_surface && !texture_current){
                LabFrame surface;
                if(labwater_volume(&g.cached,g.water_spacing,g.field,&surface)){
                    ok=labgpu_upload_volume(g.gpu,surface.parts,g.field);lab_frame_free(&surface);uploaded=true;
                    labgpu_water_surface(g.gpu,true);
                }else{LOGE("lab: water reconstruction unavailable; showing particles");g.water_surface=false;labgpu_clear_volume(g.gpu);}
            }
            if(!ok){LOGE("lab: cannot upload 3D result");return;}
            if(g.water_surface)labgpu_water_surface(g.gpu,true);
            if(volume || g.water_surface){g.volume_frame=g.frame;snprintf(g.volume_field,sizeof g.volume_field,"%s",g.field);}
            g.uploaded_frame=g.frame;g.geometry_dirty=false;g.uploads+=uploaded;
        }
        labgpu_section(g.gpu,g.section_axis,g.section_fraction,g.section_flip);
        LabViewOpts o=g.opt;o.view=g.view[0]?g.view:NULL;
        double start=draw_start;
        if(g.bench_left){o.angles=true;o.azim=g.opt.azim+g.bench_left*.5;o.elev=25;}
        if (!labgpu_draw(g.gpu,&o,g.lo,g.hi,target,x0,y0,w,h)) {
            LOGE("lab: GPU target unavailable; using software");g.use_gpu=false;g.dirty=true;return;
        }
        if(g.bench_left){glFinish();g.bench_ms+=(now_seconds()-start)*1000;if(!--g.bench_left)LOGI("lab benchmark: %u draws, %.3f ms/draw GPU-complete, %u frame reads, %u uploads",g.bench_count,g.bench_ms/g.bench_count,g.reads-g.bench_reads,g.uploads-g.bench_uploads);}
        return;
    }
    double bench_start=now_seconds();
    if(g.bench_left)g.dirty=true;
    if (w != g.tw || h != g.th) g.dirty = true;
    if (g.dirty) {
        LabFrame fr;
        char err[128];
        if (lab_read_frame(g.lf, g.frame, &fr, err, sizeof err)) {
            g.reads++;
            LabViewOpts o = g.opt;
            if(g.bench_left){o.angles=true;o.azim=g.opt.azim+g.bench_left*.5;o.elev=25;}
            /* while turning, half the size and no anti-aliasing: the picture follows the hand */
            o.w = g.rough ? w / 2 : w, o.h = g.rough ? h / 2 : h;
            o.supersample = g.rough ? 1 : 2; /* anti-aliased once the hand lets go */
            o.view = g.view[0] ? g.view : NULL;
            SwImage img;
            if (labview_render(g.lf, &fr, &o, g.lo, g.hi, &img)) {
                if (!g.tex) glGenTextures(1, &g.tex), glGenFramebuffers(1, &g.fbo);
                unsigned char *rgba = malloc((size_t)img.w * img.h * 4);
                for (int j = 0; j < img.h; j++) /* the image's first row is its top; the texture's is its bottom */
                    for (int i = 0; i < img.w; i++) {
                        const unsigned char *s = img.rgb + 3 * ((size_t)(img.h - 1 - j) * img.w + i);
                        unsigned char *d = rgba + 4 * ((size_t)j * img.w + i);
                        d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
                    }
                glBindTexture(GL_TEXTURE_2D, g.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.tex, 0);
                free(rgba);
                g.tw = w, g.th = h;
                g.opt.w = img.w, g.opt.h = img.h; /* the texture's size, for the blit below */
                sw_image_free(&img);
            }
            lab_frame_free(&fr);
        }
        g.dirty = false;
    }
    if (!g.tex) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(0, 0, g.opt.w, g.opt.h, x0, y0, x0 + w, y0 + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    if(g.bench_left){glFinish();g.bench_ms+=(now_seconds()-bench_start)*1000;if(!--g.bench_left)LOGI("lab benchmark CPU: %u draws, %.3f ms/draw complete, %u frame reads",g.bench_count,g.bench_ms/g.bench_count,g.reads-g.bench_reads);}
}

static bool onoff(int argc, char **argv, bool *v) {
    if (argc < 3) {
        *v = !*v;
        return true;
    }
    *v = str_ieq(argv[2], "on") || str_ieq(argv[2], "1") || str_ieq(argv[2], "true");
    return true;
}

void labapp_command(int argc, char **argv) {
    if (argc < 2 || str_ieq(argv[1], "help")) {
        LOGI("lab open FILE.lab | lab run SCENARIO.json | lab close | lab field NAME | lab frame N | lab play | lab pause | lab next | lab prev");
        LOGI("lab fps N | lab view iso|top|front|left|right|back | lab orbit AZIM ELEV | lab zoom F | lab focus X Y Z|off | lab iso LEVEL | lab mesh | lab mirror | lab meshbelow | lab cmap NAME");
        LOGI("lab range LO HI | lab range auto | lab info | lab renderer gpu|cpu | lab section x|y|z FRACTION|off|flip | lab bench N | lab benchframes N | lab surface on|off");
        return;
    }
    const char *c = argv[1];
    if (str_ieq(c, "open") && argc >= 3) {
        labapp_open(argv[2]);
        return;
    }
    if (str_ieq(c, "run") && argc >= 3) {
        start_run(argv[2]);
        return;
    }
    if (!g.active) {
        LOGE("lab: nothing open (lab open FILE.lab, or lab run SCENARIO.json)");
        return;
    }
    if (str_ieq(c,"renderer") && argc>=3) {
        if (!str_ieq(argv[2],"cpu") && !str_ieq(argv[2],"gpu")) {LOGE("lab renderer gpu|cpu");return;}
        g.use_gpu=str_ieq(argv[2],"gpu");g.dirty=true;
    }
    else if (str_ieq(c,"surface") && g.water_spacing>0) {onoff(argc,argv,&g.water_surface);g.geometry_dirty=g.dirty=true;}
    else if (str_ieq(c,"fit")) {labgpu_refit(g.gpu);g.opt.zoom=1;g.opt.look_at_given=false;g.geometry_dirty=true;}
    else if (str_ieq(c,"iso") && argc>=3 && g.iso_field[0]) /* lab iso LEVEL: the vortex surfaces' level, in the iso field's unit */
        g.iso_level=atof(argv[2]),g.volume_frame=-1,g.geometry_dirty=g.dirty=true;
    else if (str_ieq(c,"zoom") && argc>=3 && atof(argv[2])>0) g.opt.zoom=CLAMP(atof(argv[2]),0.2,200.0),g.dirty=true;
    else if (str_ieq(c,"focus") && argc>=3) { /* lab focus X Y Z (m) | off: the point the 3D view turns about */
        if (str_ieq(argv[2],"off")) g.opt.look_at_given=false;
        else if (argc>=5) g.opt.look_at_given=true,g.opt.look_at[0]=atof(argv[2]),g.opt.look_at[1]=atof(argv[3]),g.opt.look_at[2]=atof(argv[4]);
        else {LOGE("lab focus X Y Z | off");return;}
        g.dirty=true;
    }
    else if ((str_ieq(c,"bench") || str_ieq(c,"benchframes")) && argc>=3 && g.native3d) {g.bench_frames=str_ieq(c,"benchframes");g.bench_left=g.bench_count=CLAMP(atoi(argv[2]),1,300);g.bench_ms=0;g.bench_reads=g.reads;g.bench_uploads=g.uploads;g.playing=false;g.rough=false;}
    else if (str_ieq(c,"section") && argc>=3) {
        if (!labapp_native3d()) {LOGE("lab: sections require a supported 3D result with the GPU renderer");return;}
        if(str_ieq(argv[2],"off"))g.section_axis=-1;
        else if(str_ieq(argv[2],"flip"))g.section_flip=!g.section_flip;
        else if(argc>=4 && strlen(argv[2])==1 && strchr("xyz",argv[2][0])) {g.section_axis=(int)(strchr("xyz",argv[2][0])-"xyz");g.section_fraction=CLAMP(atof(argv[3]),0,1);}
        else {LOGE("lab section x|y|z FRACTION | off | flip");return;}
        g.geometry_dirty=g.dirty=true;
    }
    else if (str_ieq(c, "close")) labapp_close(), LOGI("lab: closed");
    else if (str_ieq(c, "field") && argc >= 3) set_field(argv[2]);
    else if (str_ieq(c, "frame") && argc >= 3) g.frame = CLAMP(atoi(argv[2]), 0, g.nframes - 1), g.playing = false, g.dirty = true;
    else if (str_ieq(c, "play")) g.playing = true;
    else if (str_ieq(c, "pause")) g.playing = false;
    else if (str_ieq(c, "next")) g.frame = (g.frame + 1) % g.nframes, g.playing = false, g.dirty = true;
    else if (str_ieq(c, "prev")) g.frame = (g.frame + g.nframes - 1) % g.nframes, g.playing = false, g.dirty = true;
    else if (str_ieq(c, "fps") && argc >= 3) g.fps = CLAMP(atof(argv[2]), 0.5, 60.0);
    else if (str_ieq(c, "view") && argc >= 3) snprintf(g.view, sizeof g.view, "%s", argv[2]), g.opt.angles = false, g.dirty = true;
    else if (str_ieq(c, "orbit") && argc >= 4) g.opt.angles = true, g.opt.azim = atof(argv[2]), g.opt.elev = atof(argv[3]), g.dirty = true;
    else if (str_ieq(c, "mesh")) onoff(argc, argv, &g.opt.mesh), g.geometry_dirty = g.dirty = true;
    else if (str_ieq(c, "mirror")) onoff(argc, argv, &g.opt.mirror), g.dirty = true;
    else if (str_ieq(c, "meshbelow")) onoff(argc, argv, &g.opt.mesh_below), g.opt.mirror = g.opt.mirror || g.opt.mesh_below, g.dirty = true;
    else if (str_ieq(c, "light")) onoff(argc, argv, &g.opt.light), g.dirty = true;
    else if (str_ieq(c, "cmap") && argc >= 3) {
        int m = !strcmp(argv[2], "gray") ? 100 : !strcmp(argv[2], "ink") ? 101 : !strcmp(argv[2], "ember") ? 102 : sw_colormap_find(argv[2]);
        if (m < 0) LOGE("lab: unknown colour map %s", argv[2]);
        else g.opt.cmap = m, g.dirty = true;
    } else if (str_ieq(c, "range") && argc >= 3) {
        if (str_ieq(argv[2], "auto")) g.auto_range = true, set_field(g.field);
        else if (argc >= 4) g.auto_range = false, g.lo = atof(argv[2]), g.hi = atof(argv[3]), g.dirty = true;
    } else if (str_ieq(c, "info")) {
        LOGI("lab: %s", g.path);
        LOGI("  frame %d/%d, %s, playback %.0f fps", g.frame + 1, g.nframes, g.playing ? "playing" : "paused", g.fps);
        LOGI("  renderer %s, frame reads %u, uploads %u, section axis %d fraction %.3f flip %d",g.native3d && g.use_gpu?"retained GPU 3D":"software",g.reads,g.uploads,g.section_axis,g.section_fraction,g.section_flip);
        if(g.water_spacing>0)LOGI("  water display %s",g.water_surface?"surface":"particles");
        LOGI("  %s", lab_header(g.lf));
        LOGI("  %d frames, field %s from %.6g to %.6g %s", g.nframes, g.field, g.lo, g.hi, g.unit);
    } else LOGE("lab: unknown '%s' (lab help)", c);
}

/* ---- for the panels ---- */
int labapp_frame(void) { return g.frame; }
int labapp_nframes(void) { return g.active ? g.nframes : 0; }
double labapp_frame_time(int f) { return g.lf ? lab_frame_time(g.lf, f) : 0; }
void labapp_set_frame(int f) {
    if (!g.active) return;
    g.frame = f < 0 ? 0 : f >= g.nframes ? g.nframes - 1 : f;
    g.dirty = true;
}
bool labapp_playing(void) { return g.playing; }
void labapp_set_playing(bool on) { g.playing = on && g.nframes > 1; }
const char *labapp_field(void) { return g.field; }
void labapp_set_field(const char *name) {
    if (g.active) set_field(name);
}
int labapp_field_names(char names[][48], int max) {
    if (!g.active) return 0;
    LabFrame fr;
    char err[128];
    int n = 0;
    bool borrowed = g.native3d && g.cached_frame == g.frame;
    if (borrowed) fr = g.cached;
    else if (!lab_read_frame(g.lf, g.frame, &fr, err, sizeof err)) return 0;
    for (int p = 0; p < fr.nparts; p++)
        for (int k = 0; k < fr.parts[p].nfields && n < max; k++) {
            const char *f = fr.parts[p].fields[k].name;
            if (!strcmp(f, "solid") || !strcmp(f, "material") || !strcmp(f, "body")) continue;
            bool dup = false;
            for (int q = 0; q < n; q++) dup = dup || !strcmp(names[q], f);
            if (!dup) snprintf(names[n++], 48, "%s", f);
        }
    if (!borrowed) lab_frame_free(&fr);
    return n;
}
const char *labapp_domain(void) { return g.active ? g.domain : ""; }
const char *labapp_time_unit(void) { return g.active ? g.time_unit : ""; }
bool labapp_running(void) { return g.running; }
const char *labapp_run_name(void) { return g.run_name; }
const char *labapp_run_error(void) { return g.running ? "" : g.run_err; }
double labapp_fps(void) { return g.fps; }
void labapp_run(const char *path) { start_run(path); }

void labapp_select(const char *scenario) {
    const char *base = strrchr(scenario, '/');
    base = base ? base + 1 : scenario;
    char result[1200];
    snprintf(result, sizeof result, "%s/%.*s.lab", labapp_results_dir(), (int)strcspn(base, "."), base);
    struct stat st;
    if (stat(result, &st) == 0) labapp_open(result);
    else start_run(scenario);
}

/* the value a dotted path names in a scenario ("bodies.1.velocity_m_s.2"), or NULL */
static JsonValue *path_get(JsonValue *root, const char *path) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    JsonValue *v = root;
    for (char *tok = strtok(buf, "."); tok && v; tok = strtok(NULL, ".")) {
        if (v->type == JSON_ARRAY) v = json_at(v, (size_t)atoi(tok));
        else v = json_get(v, tok);
    }
    return v;
}

int labapp_controls(LabControl *out, int max) {
    const JsonValue *cs = g.scenario ? json_get(g.scenario, "controls") : NULL;
    int n = 0;
    for (size_t i = 0; cs && i < json_len(cs) && n < max; i++) {
        const JsonValue *c = json_at(cs, i);
        JsonValue *v = path_get(g.scenario, json_get_str(c, "path", ""));
        if (!v || v->type != JSON_NUMBER) continue;
        LabControl *L = &out[n++];
        snprintf(L->label, sizeof L->label, "%s", json_get_str(c, "label", json_get_str(c, "path", "")));
        L->value = v->u.number, L->min = json_get_num(c, "min", 0), L->max = json_get_num(c, "max", 1);
        L->log = json_get_bool(c, "log", false);
    }
    return n;
}

void labapp_control_set(int i, double value) {
    const JsonValue *cs = g.scenario ? json_get(g.scenario, "controls") : NULL;
    if (!cs || i < 0 || (size_t)i >= json_len(cs)) return;
    JsonValue *v = path_get(g.scenario, json_get_str(json_at(cs, (size_t)i), "path", ""));
    if (v && v->type == JSON_NUMBER) v->u.number = value;
}

/* runs the open result's scenario again with its controls as they are now */
void labapp_rerun(void) {
    if (!g.scenario || g.running) return;
    const char *home = getenv("HOME");
    char path[1100];
    snprintf(path, sizeof path, "%s/NAVIER-Projects/lab/%.*s-edited.json", home ? home : "/tmp",
             (int)strcspn(strrchr(g.scenario_path, '/') ? strrchr(g.scenario_path, '/') + 1 : g.scenario_path, "."),
             strrchr(g.scenario_path, '/') ? strrchr(g.scenario_path, '/') + 1 : g.scenario_path);
    if (!json_write_file(path, g.scenario, JSON_PRETTY)) {
        LOGE("lab: cannot write %s", path);
        return;
    }
    start_run(path);
}

/* ---- following the results folder ---- */
const char *labapp_results_dir(void) {
    static char dir[1024];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof dir, "%s/NAVIER-Projects/lab", home ? home : "/tmp");
    return dir;
}

void labapp_follow(double dt) {
    static double acc;
    static time_t seen; /* the newest modification time already handled */
    static char pending[1024];
    static off_t pending_size = -1;
    acc += dt;
    if (acc < 1.0) return;
    acc = 0;
    if (!seen) seen = time(NULL); /* only results that appear from now on */
    DIR *d = opendir(labapp_results_dir());
    if (!d) return;
    struct dirent *e;
    char best[1024] = "";
    time_t bt = seen;
    off_t bsize = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".lab")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", labapp_results_dir(), e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && st.st_mtime > bt) bt = st.st_mtime, bsize = st.st_size, snprintf(best, sizeof best, "%s", p);
    }
    closedir(d);
    if (!best[0]) return;
    /* open it once its size has held for a second: the writer has finished */
    if (strcmp(best, pending) || bsize != pending_size) {
        snprintf(pending, sizeof pending, "%s", best);
        pending_size = bsize;
        return;
    }
    seen = bt;
    pending[0] = 0, pending_size = -1;
    if (!g.running && strcmp(best, g.path)) labapp_open(best);
}

int labapp_recent(char paths[][1024], char titles[][120], int max) {
    DIR *d = opendir(labapp_results_dir());
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    time_t tm[64];
    while ((e = readdir(d)) && n < max && n < 64) {
        size_t k = strlen(e->d_name);
        if (k < 5 || strcmp(e->d_name + k - 4, ".lab")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", labapp_results_dir(), e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        char err[64];
        LabFile *lf = lab_open(p, err, sizeof err);
        if (!lf) continue;
        JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL);
        snprintf(titles[n], 120, "%s", h ? json_get_str(h, "title", e->d_name) : e->d_name);
        json_free(h);
        lab_close_file(lf);
        snprintf(paths[n], 1024, "%s", p);
        tm[n++] = st.st_mtime;
    }
    closedir(d);
    for (int i = 1; i < n; i++) /* newest first */
        for (int j = i; j > 0 && tm[j] > tm[j - 1]; j--) {
            time_t t = tm[j];
            tm[j] = tm[j - 1], tm[j - 1] = t;
            char pb[1024], tb[120];
            memcpy(pb, paths[j], 1024), memcpy(paths[j], paths[j - 1], 1024), memcpy(paths[j - 1], pb, 1024);
            memcpy(tb, titles[j], 120), memcpy(titles[j], titles[j - 1], 120), memcpy(titles[j - 1], tb, 120);
        }
    return n;
}

bool labapp_legend(double *lo, double *hi, int *cmap, const char **field, const char **unit) {
    if (!g.active || !g.field[0]) return false;
    *lo = g.lo, *hi = g.hi, *field = g.field, *unit = g.unit;
    int c = g.opt.cmap;
    if (c == STYLE_AUTO) {
        c = style_pick(g.lo, g.hi);
        if (c == SW_CMAP_LAB && !strcmp(g.domain, "impact")) c = SW_CMAP_LAB_SOLID; /* a solid wears the solid palette */
    }
    *cmap = c;
    return true;
}

bool labapp_native3d(void) { return g.active && g.native3d && g.use_gpu; }
int labapp_section_axis(void) { return g.section_axis; }
bool labapp_section_flipped(void) { return g.section_flip; }
double labapp_section_fraction(void) { return g.section_fraction; }
void labapp_section_set(int axis,double fraction) {g.section_axis=axis;g.section_fraction=CLAMP(fraction,0,1);g.geometry_dirty=g.dirty=true;}

bool labapp_is_water(void){return g.active && g.water_spacing>0;}
bool labapp_water_surface(void){return g.water_surface;}
