/* labgpu.c - the lab's native 3D renderer (labgpu.h): one lit world for every result.
 *
 * Passes, each frame:
 *   shadow   the retained geometry seen from the key light into a depth map (2048^2)
 *   scene    room floor, surfaces, particles and lines into a 4x multisampled HDR target, lit by a hemisphere sky,
 *            the shadowed key light, a rim and a specular lobe; resolved to colour and depth textures
 *   volume   a computed 3D field ray-marched through its box, stopped by the scene's depth so that bodies inside a
 *            field hide what is behind them; shaded by the field's own gradient so that structures read as shapes
 *   compose  ambient occlusion from the scene depth, the volume over the scene, a tone curve that leaves the legend's
 *            colours exact below its knee, a slight vignette, dither; written into the app's framebuffer
 *
 * Lighting, occlusion, shadows and the room are presentation, not computed radiance (docs/lab/README.md). The colour of
 * a surface or sample is the legend's colour of its stored value; light only scales it. */
#include "labgpu.h"
#include "labvolume_shader.h"
#include "glutil.h"
#include "lab/style.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { SHADOW = 2048 };

struct LabGpu {
    GLuint geo, bg, vol, comp, shadow;       /* programs */
    GLuint vao[3], vbo[3], empty_vao;
    int n[3];
    GLuint pal[2]; int pal_map[2];            /* palettes: the field's map, and the solid one for bodies */
    GLuint volume_tex[2];
    int w, h;                                 /* targets */
    GLuint ms_fbo, ms_color, ms_depth, r_fbo, r_color, r_depth, v_fbo, v_color, sh_fbo, sh_depth;
    double lo[3], hi[3];
    bool framed, volume, water_surface, iso;
    double origin[3], extent[3], cell[3], fraction;
    int axis, flip;
};

/* ---- shaders ---------------------------------------------------------------------------------------------------- */

#define GLSL_CAMERA \
    "uniform vec3 eye,right,up,forward;uniform vec2 size;uniform float focal,nearz,farz;\n" \
    "float windowDepth(float z){float A=(farz+nearz)/(farz-nearz),B=-2.*farz*nearz/(farz-nearz);return .5*(A+B/max(z,nearz))+.5;}\n"

#define GLSL_LIGHT \
    "uniform sampler2DShadow shadowMap;uniform vec3 lightDir,lightRight,lightUp,lightCentre;uniform float lightRadius;\n" \
    "const vec3 SKY=vec3(.50,.56,.66),GROUND=vec3(.20,.19,.18),SUN=vec3(1.,.96,.90);\n" \
    "uniform int shadowsOff;\n" \
    "float shadowAt(vec3 p,vec3 n){if(shadowsOff!=0)return 1.;\n" \
    "  vec3 q=p+n*lightRadius*.004-lightCentre;\n" \
    "  vec3 s=vec3(dot(q,lightRight),dot(q,lightUp),dot(q,-lightDir))/lightRadius*.5+.5;\n" \
    "  if(any(lessThan(s.xy,vec2(0.)))||any(greaterThan(s.xy,vec2(1.))))return 1.;\n" \
    "  float t=1./2048.,sum=0.;for(int i=-2;i<=2;i++)for(int j=-2;j<=2;j++)sum+=texture(shadowMap,vec3(s.xy+vec2(i,j)*t*1.5,s.z-.0015));\n" \
    "  return sum/25.;}\n" \
    "vec3 lit(vec3 albedo,vec3 n,vec3 v,float shadow,float gloss){\n" \
    "  float hemi=.5+.5*n.z;vec3 ambient=mix(GROUND,SKY,hemi)*.62;\n" \
    "  float d=max(dot(n,lightDir),0.);vec3 h=normalize(lightDir+v);\n" \
    "  float spec=pow(max(dot(n,h),0.),gloss)*(gloss+8.)/(8.*3.14159)*.10;\n" \
    "  float rim=pow(1.-max(dot(n,v),0.),3.)*.18;\n" \
    "  return albedo*(ambient+SUN*.78*d*shadow)+SUN*spec*shadow+SKY*rim;}\n" \
    "vec3 toLinear(vec3 c){return pow(c,vec3(2.2));}\n"

static const char *vs_geo =
    "#version 410 core\n"
    "layout(location=0) in vec3 pos;layout(location=1) in float value;layout(location=2) in float radius;layout(location=3) in float hasfield;layout(location=4) in vec3 vnormal;\n"
    GLSL_CAMERA
    "uniform float pointpx;\n"
    "out vec3 world;out float scalar;out float field;out float rr;out float zz;out vec3 smoothN;\n"
    "void main(){vec3 q=pos-eye;float z=dot(q,forward);world=pos;scalar=value;field=hasfield;zz=z;smoothN=vnormal;\n"
    "  rr=max(radius,pointpx*max(z,nearz)/focal);gl_PointSize=2.*rr*focal/max(z,nearz);\n"
    "  float A=(farz+nearz)/(farz-nearz),B=-2.*farz*nearz/(farz-nearz);\n"
    "  gl_Position=vec4(2.*focal/size.x*dot(q,right),2.*focal/size.y*dot(q,up),A*z+B,z);}\n";

static const char *fs_geo =
    "#version 410 core\n"
    "in vec3 world;in float scalar;in float field;in float rr;in float zz;in vec3 smoothN;out vec4 frag;\n"
    GLSL_CAMERA GLSL_LIGHT
    "uniform sampler3D coverageTex;uniform int waterDroplets;uniform vec3 volumeOrigin,volumeExtent;\n"
    "uniform sampler2D palette;uniform vec2 range;uniform int mode;\n"
    "void main(){\n"
    "  if(mode==1&&waterDroplets!=0&&texture(coverageTex,(world-volumeOrigin)/volumeExtent).r>=.5)discard;\n"
    "  vec3 n,p=world;float z=zz;\n"
    "  if(mode==1){vec2 d=2.*gl_PointCoord-1.;float r=dot(d,d);if(r>1.)discard;float h=sqrt(1.-r);\n"
    "    n=normalize(right*d.x-up*d.y-forward*h);p=world+n*rr;z-=rr*h;}\n"
    "  else n=dot(smoothN,smoothN)>.25?normalize(smoothN):normalize(cross(dFdx(world),dFdy(world)));\n"
    "  vec3 v=normalize(eye-p);if(dot(n,v)<0.)n=-n;\n"
    "  vec3 c=field>0.?texture(palette,vec2(clamp((scalar-range.x)/max(range.y-range.x,1e-20),0.,1.),.5)).rgb:vec3(.80,.80,.78);\n"
    "  float gloss=mode==1?64.:32.;\n"
    "  if(field<0.&&field>-1.5)c=vec3(.13,.18,.22);\n"
    "  if(field<-1.5){int look=int(-field+.5)-1;\n"   /* labscene.h LOOK_*: materials, not fields */
    "    c=look==1?vec3(.86,.50,.30):look==2?vec3(.80,.22,.20):look==3?vec3(.22,.38,.80):look==4?vec3(.64,.66,.69):look==5?vec3(.82,.83,.85):look==7?vec3(.72,.09,.11):look==8?vec3(.93,.74,.66):vec3(.72,.86,.92);\n"
    "    gloss=look==1?90.:look==6?160.:look==7?10.:look==8?40.:60.;}\n"
    "  c=toLinear(c);\n"
    "  if(mode==2)frag=vec4(c*.9+.05,1.);\n"
    "  else frag=vec4(lit(c,n,v,shadowAt(p,n),gloss),1.);\n"
    "  gl_FragDepth=windowDepth(z)-(mode==2?.000005:0.);}\n";

static const char *vs_full = "#version 410 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}\n";

static const char *fs_bg =
    "#version 410 core\n"
    "out vec4 frag;\n"
    GLSL_CAMERA GLSL_LIGHT
    "uniform float floorz,span;uniform int light;uniform vec2 offset;\n"
    "void main(){vec2 p=gl_FragCoord.xy;float t=p.y/size.y;\n"
    "  vec3 c=toLinear(mix(vec3(27,32,47)/255.,vec3(10,12,20)/255.,t));if(light!=0)c=toLinear(mix(vec3(.90,.91,.93),vec3(.98),t));\n"
    "  vec3 ray=forward+right*(p.x-size.x*.5)/focal+up*(p.y-size.y*.5)/focal;float z=(floorz-eye.z)/ray.z;gl_FragDepth=1.;\n"
    "  if(z>nearz&&z<farz){vec3 w=eye+ray*z;float step=span/10.;\n"
    "    vec2 g=abs(fract(w.xy/step-.5)-.5)/max(fwidth(w.xy/step),vec2(.0001));float grid=1.-min(min(g.x,g.y),1.);\n"
    "    float fade=exp(-length(w.xy-lightCentre.xy)/max(span,1e-6)*.45);\n"
    "    vec3 floorc=light!=0?vec3(.80,.81,.83):vec3(.030,.037,.055);\n"
    "    vec3 f=floorc*(.55+.75*shadowAt(w,vec3(0,0,1)))+vec3(.010,.018,.024)*grid;\n"
    "    c=mix(c,f,.85*fade);gl_FragDepth=windowDepth(z);}\n"
    "  frag=vec4(c,1.);}\n";

static const char *vs_shadow =
    "#version 410 core\n"
    "layout(location=0) in vec3 pos;layout(location=2) in float radius;\n"
    "uniform vec3 lightDir,lightRight,lightUp,lightCentre;uniform float lightRadius,minRadius;out float rr;\n"
    "void main(){vec3 q=pos-lightCentre;rr=max(radius,minRadius);gl_PointSize=max(rr/lightRadius*2048.,1.);\n"
    "  gl_Position=vec4(dot(q,lightRight)/lightRadius,dot(q,lightUp)/lightRadius,dot(q,-lightDir)/lightRadius,1.);}\n";
static const char *fs_shadow =
    "#version 410 core\n"
    "in float rr;uniform int mode;uniform float lightRadius;\n"
    "void main(){float z=gl_FragCoord.z;if(mode==1){vec2 d=2.*gl_PointCoord-1.;float r=dot(d,d);if(r>1.)discard;z-=sqrt(1.-r)*rr/lightRadius*.5;}\n"
    "  gl_FragDepth=z;}\n";

static const char *fs_comp =
    "#version 410 core\n"
    "out vec4 frag;uniform sampler2D sceneColor,sceneDepth,volumeColor;uniform vec2 size,offset;uniform float nearz,farz,focal,aoRadius;uniform int light,aoOff;\n"
    "float linearZ(float d){float A=(farz+nearz)/(farz-nearz),B=-2.*farz*nearz/(farz-nearz);return B/(2.*d-1.-A);}\n"
    "vec3 viewPos(vec2 px){float d=texture(sceneDepth,px/size).r;float z=linearZ(d);return vec3((px-size*.5)/focal*z,z);}\n"
    "float tone(float x){const float k=.82;return x<k?x:k+(1.-k)*(1.-exp(-(x-k)/(1.-k)));}\n"
    "void main(){vec2 px=gl_FragCoord.xy-offset;vec2 uv=px/size;\n"
    "  vec3 c=texture(sceneColor,uv).rgb;float d=texture(sceneDepth,uv).r;\n"
    "  if(d<1.&&aoOff==0){\n"   /* ambient occlusion: how much of the hemisphere above this point nearby surfaces hide */
    "    vec3 P=viewPos(px);vec3 N=normalize(cross(dFdx(P),dFdy(P)));if(N.z>0.)N=-N;\n"
    "    float R=aoRadius,occ=0.;float pr=clamp(R*focal/P.z,3.,60.);\n"
    "    for(int i=0;i<16;i++){float a=float(i)*2.39996+fract(sin(dot(px,vec2(12.9898,78.233)))*43758.55)*6.2832;\n"
    "      float r=pr*sqrt((float(i)+.5)/16.);vec3 S=viewPos(px+r*vec2(cos(a),sin(a)));vec3 D=S-P;float l=length(D);\n"
    "      occ+=max(dot(N,D/max(l,1e-9))-.05,0.)*clamp(1.-l/R*.5,0.,1.);}\n"
    "    c*=1.-.75*clamp(occ/16.*1.8,0.,1.);}\n"
    "  vec4 v=texture(volumeColor,uv);c=v.rgb+(1.-v.a)*c;\n"
    "  c=vec3(tone(c.r),tone(c.g),tone(c.b));c=pow(c,vec3(1./2.2));\n"
    "  if(light==0){vec2 q=uv-.5;c*=1.-.28*dot(q,q);}\n"
    "  c+=(fract(sin(dot(px,vec2(91.345,47.853)))*24634.63)-.5)/255.;\n"
    "  frag=vec4(c,1.);}\n";

/* ---- helpers ---------------------------------------------------------------------------------------------------- */

static void f1(GLuint p, const char *n, float f) { glUniform1f(glGetUniformLocation(p, n), f); }
static void i1(GLuint p, const char *n, int i) { glUniform1i(glGetUniformLocation(p, n), i); }
static void v2(GLuint p, const char *n, double a, double b) { glUniform2f(glGetUniformLocation(p, n), (float)a, (float)b); }
static void v3(GLuint p, const char *n, const double *v) { glUniform3f(glGetUniformLocation(p, n), (float)v[0], (float)v[1], (float)v[2]); }

static GLuint tex2d(GLenum internal, GLenum format, GLenum type, int w, int h, GLenum filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void free_targets(LabGpu *g) {
    GLuint fb[3] = {g->ms_fbo, g->r_fbo, g->v_fbo}, rb[2] = {g->ms_color, g->ms_depth}, tx[3] = {g->r_color, g->r_depth, g->v_color};
    glDeleteFramebuffers(3, fb), glDeleteRenderbuffers(2, rb), glDeleteTextures(3, tx);
    g->ms_fbo = g->r_fbo = g->v_fbo = g->ms_color = g->ms_depth = g->r_color = g->r_depth = g->v_color = 0;
    g->w = g->h = 0;
}

static bool make_targets(LabGpu *g, int w, int h) {
    if (g->w == w && g->h == h) return true;
    free_targets(g);
    glGenFramebuffers(1, &g->ms_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->ms_fbo);
    glGenRenderbuffers(1, &g->ms_color);
    glBindRenderbuffer(GL_RENDERBUFFER, g->ms_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA16F, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->ms_color);
    glGenRenderbuffers(1, &g->ms_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g->ms_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->ms_depth);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &g->r_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->r_fbo);
    g->r_color = tex2d(GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_NEAREST);
    g->r_depth = tex2d(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, w, h, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->r_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g->r_depth, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &g->v_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->v_fbo);
    g->v_color = tex2d(GL_RGBA16F, GL_RGBA, GL_FLOAT, w, h, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->v_color, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) { free_targets(g); return false; }
    g->w = w, g->h = h;
    return true;
}

/* ---- life cycle ------------------------------------------------------------------------------------------------- */

LabGpu *labgpu_create(void) {
    LabGpu *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->axis = -1, g->pal_map[0] = g->pal_map[1] = -1;
    g->geo = gl_program("lab surfaces", vs_geo, NULL, fs_geo);
    g->bg = gl_program("lab room", vs_full, NULL, fs_bg);
    g->vol = gl_program("lab computed volume", vs_full, NULL, volume_fs);
    g->comp = gl_program("lab compose", vs_full, NULL, fs_comp);
    g->shadow = gl_program("lab shadow", vs_shadow, NULL, fs_shadow);
    if (!g->geo || !g->bg || !g->vol || !g->comp || !g->shadow) { labgpu_destroy(g); return NULL; }
    glGenVertexArrays(3, g->vao), glGenBuffers(3, g->vbo), glGenVertexArrays(1, &g->empty_vao);
    glGenTextures(2, g->pal), glGenTextures(2, g->volume_tex);
    for (int i = 0; i < 3; i++) {
        glBindVertexArray(g->vao[i]);
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo[i]);
        int count[5] = {3, 1, 1, 1, 3};
        size_t off[5] = {offsetof(LabVertex, p), offsetof(LabVertex, value), offsetof(LabVertex, radius), offsetof(LabVertex, field), offsetof(LabVertex, n)};
        for (int a = 0; a < 5; a++) {
            glEnableVertexAttribArray((GLuint)a);
            glVertexAttribPointer((GLuint)a, count[a], GL_FLOAT, GL_FALSE, sizeof(LabVertex), (void *)off[a]);
        }
    }
    glBindVertexArray(0);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g->pal[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    /* the key light's depth map, compared in hardware (2 x 2 filtered, 5 x 5 taps in the shader) */
    g->sh_depth = tex2d(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, SHADOW, SHADOW, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &g->sh_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->sh_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g->sh_depth, 0);
    glDrawBuffer(GL_NONE), glReadBuffer(GL_NONE);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) { labgpu_destroy(g); return NULL; }
    return g;
}

void labgpu_destroy(LabGpu *g) {
    if (!g) return;
    free_targets(g);
    GLuint prog[5] = {g->geo, g->bg, g->vol, g->comp, g->shadow};
    for (int i = 0; i < 5; i++) glDeleteProgram(prog[i]);
    glDeleteVertexArrays(3, g->vao), glDeleteVertexArrays(1, &g->empty_vao), glDeleteBuffers(3, g->vbo);
    glDeleteTextures(2, g->pal), glDeleteTextures(2, g->volume_tex), glDeleteTextures(1, &g->sh_depth);
    glDeleteFramebuffers(1, &g->sh_fbo);
    free(g);
}

bool labgpu_upload(LabGpu *g, const LabScene *s) {
    if (!g) return false;
    const LabVertex *v[3] = {s->tri, s->points, s->lines};
    size_t n[3] = {s->ntri, s->npoints, s->nlines};
    for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n[i] * sizeof(LabVertex)), v[i], GL_STATIC_DRAW);
        g->n[i] = (int)n[i];
    }
    if (!g->framed && s->ntri + s->npoints + s->nlines > 0) {
        memcpy(g->lo, s->lo, sizeof g->lo), memcpy(g->hi, s->hi, sizeof g->hi);
        g->framed = true;
    }
    return true;
}

void labgpu_clear_volume(LabGpu *g) {
    if (g) g->volume = g->water_surface = g->iso = false;
}

void labgpu_refit(LabGpu *g) {
    if (g) g->framed = false;
}

void labgpu_section(LabGpu *g, int axis, double fraction, bool flip) {
    if (g) g->axis = axis, g->fraction = fraction, g->flip = flip;
}

static bool upload_volume(LabGpu *g, const LabPart *p, const char *field, const char *iso_field, double level);
bool labgpu_upload_volume(LabGpu *g, const LabPart *p, const char *field) { return upload_volume(g, p, field, NULL, 0); }
bool labgpu_upload_volume_iso(LabGpu *g, const LabPart *p, const char *field, const char *iso_field, double level) {
    return upload_volume(g, p, field, iso_field, level);
}
static bool upload_volume(LabGpu *g, const LabPart *p, const char *field, const char *iso_field, double level) {
    const LabBlock *b = p->blocks;
    const LabField *f = lab_find_field(p, field), *m = lab_find_field(p, "material"), *iso = iso_field ? lab_find_field(p, iso_field) : NULL;
    if (!g || !f) return false;
    GLint maxsize;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maxsize);
    for (int k = 0; k < 3; k++)
        if (b->n[k] > maxsize) return false;
    const LabField *coverage = iso ? NULL : lab_find_field(p, "coverage");
    float *mask = coverage ? NULL : calloc(f->count, sizeof(float));
    if (!coverage && !mask) return false;
    if (iso && level > 0) { /* 0.5 where the field crosses its level: the shader's surface threshold */
        m = NULL;
        for (size_t i = 0; i < f->count && i < iso->count; i++) {
            double v = iso->data[i] / (2 * level);
            mask[i] = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
        }
    }
    if (m && mask) { /* a body's occupancy, box-filtered once so that its surface is traced smooth, not in voxel steps */
        int nx = b->n[0], ny = b->n[1], nz = b->n[2];
        for (int z = 0; z < nz; z++)
            for (int y = 0; y < ny; y++)
                for (int x = 0; x < nx; x++) {
                    float s = 0;
                    int c = 0;
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int X = x + dx, Y = y + dy, Z = z + dz;
                                if (X < 0 || Y < 0 || Z < 0 || X >= nx || Y >= ny || Z >= nz) continue;
                                s += m->data[(size_t)X + (size_t)nx * ((size_t)Y + (size_t)ny * Z)] > 0 ? 1.f : 0.f, c++;
                            }
                    mask[(size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * z)] = s / c;
                }
    }
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE1 + i);
        glBindTexture(GL_TEXTURE_3D, g->volume_tex[i]);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR), glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE), glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, b->n[0], b->n[1], b->n[2], 0, GL_RED, GL_FLOAT, i == 0 ? f->data : coverage ? coverage->data : mask);
    }
    free(mask);
    glActiveTexture(GL_TEXTURE0);
    for (int k = 0; k < 3; k++) {
        g->origin[k] = b->origin[k], g->cell[k] = b->dx[k], g->extent[k] = b->dx[k] * b->n[k];
        if (!g->framed) g->lo[k] = g->origin[k], g->hi[k] = g->origin[k] + g->extent[k];
    }
    g->framed = true, g->volume = true;
    g->iso = iso && level > 0;
    return true;
}

void labgpu_water_surface(LabGpu *g, bool enabled) {
    if (g) {
        g->water_surface = enabled;
        if (enabled) g->volume = true;
    }
}

/* ---- drawing ---------------------------------------------------------------------------------------------------- */

static void camera_uniforms(GLuint p, const SwCamera *c, int w, int h, double nearz, double farz) {
    v3(p, "eye", c->eye), v3(p, "right", c->right), v3(p, "up", c->up), v3(p, "forward", c->forward);
    f1(p, "focal", (float)(h * .5 / tan(c->fov_deg * M_PI / 360))), f1(p, "nearz", (float)nearz), f1(p, "farz", (float)farz);
    v2(p, "size", w, h);
}

typedef struct Light {
    double dir[3], right[3], up[3], centre[3], radius;
} Light;

static int shadows_off = -1;
static void light_uniforms(GLuint p, const Light *L, int unit) {
    if (shadows_off < 0) shadows_off = getenv("NAVIER_NO_SHADOWS") != NULL; /* diagnostics */
    i1(p, "shadowsOff", shadows_off);
    v3(p, "lightDir", L->dir), v3(p, "lightRight", L->right), v3(p, "lightUp", L->up), v3(p, "lightCentre", L->centre);
    f1(p, "lightRadius", (float)L->radius), i1(p, "shadowMap", unit);
}

static void palette(LabGpu *g, int slot, int map) {
    glBindTexture(GL_TEXTURE_2D, g->pal[slot]);
    if (g->pal_map[slot] == map) return;
    float lut[1024 * 3];
    for (int j = 0; j < 1024; j++) labview_cmap_rgb(map, j / 1023., lut + j * 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 1024, 1, 0, GL_RGB, GL_FLOAT, lut);
    g->pal_map[slot] = map;
}

bool labgpu_draw(LabGpu *g, const LabViewOpts *o, double lo, double hi, unsigned target, int x, int y, int w, int h) {
    if (!g || w < 1 || h < 1 || !make_targets(g, w, h)) return false;
    double a[3], b[3], span = 0;
    for (int k = 0; k < 3; k++) span = fmax(span, g->hi[k] - g->lo[k]);
    if (span < 1e-12) span = 1;
    for (int k = 0; k < 3; k++) {
        double centre = o->look_at_given ? o->look_at[k] : (g->lo[k] + g->hi[k]) * .5, half = fmax(g->hi[k] - g->lo[k], span * .02) * .5 / o->zoom;
        a[k] = centre - half, b[k] = centre + half;
    }
    SwCamera c = {0};
    if (o->angles) {
        double mid[3], r = 0;
        for (int k = 0; k < 3; k++) mid[k] = (a[k] + b[k]) * .5, r += (b[k] - a[k]) * (b[k] - a[k]);
        double dist = .5 * sqrt(r) / sin(M_PI / 12), az = o->azim * M_PI / 180, el = o->elev * M_PI / 180;
        double e[3] = {mid[0] + dist * cos(el) * cos(az), mid[1] + dist * cos(el) * sin(az), mid[2] + dist * sin(el)}, up[3] = {0, 0, 1};
        sw_camera_look(&c, e, mid, up, 30, w, h);
    } else if (!sw_camera_preset(&c, o->view ? o->view : "iso", a, b, 30, w, h))
        sw_camera_preset(&c, "iso", a, b, 30, w, h);
    double nearz = span * .001 / o->zoom, farz = span * 100 / o->zoom;

    /* the key light: from above, to one side, in a box around the whole scene */
    Light L = {{-.45, -.35, .82}, {0}, {0}, {0}, 0};
    double ln = sqrt(L.dir[0] * L.dir[0] + L.dir[1] * L.dir[1] + L.dir[2] * L.dir[2]);
    for (int k = 0; k < 3; k++) L.dir[k] /= ln, L.centre[k] = (g->lo[k] + g->hi[k]) * .5, L.radius += (g->hi[k] - g->lo[k]) * (g->hi[k] - g->lo[k]);
    L.centre[2] = .5 * (g->lo[2] - .025 * span + g->hi[2]);
    L.radius = .5 * sqrt(L.radius + .0025 * span * span) * 1.15 + 1e-9;
    double zup[3] = {0, 0, 1}, r0 = L.dir[1] * zup[2] - L.dir[2] * zup[1], r1 = L.dir[2] * zup[0] - L.dir[0] * zup[2], r2 = L.dir[0] * zup[1] - L.dir[1] * zup[0];
    double rn = sqrt(r0 * r0 + r1 * r1 + r2 * r2);
    L.right[0] = r0 / rn, L.right[1] = r1 / rn, L.right[2] = r2 / rn;
    L.up[0] = L.right[1] * L.dir[2] - L.right[2] * L.dir[1], L.up[1] = L.right[2] * L.dir[0] - L.right[0] * L.dir[2], L.up[2] = L.right[0] * L.dir[1] - L.right[1] * L.dir[0];

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST), blend_on = glIsEnabled(GL_BLEND), cull_on = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_BLEND), glDisable(GL_CULL_FACE), glEnable(GL_DEPTH_TEST), glDepthMask(GL_TRUE), glDepthFunc(GL_LEQUAL);
    glEnable(GL_PROGRAM_POINT_SIZE);
    int cm = o->cmap == STYLE_AUTO ? style_pick(lo, hi) : o->cmap;
    bool geometry = g->n[0] + g->n[1] + g->n[2] > 0;

    /* 1. shadow map */
    glBindFramebuffer(GL_FRAMEBUFFER, g->sh_fbo);
    glViewport(0, 0, SHADOW, SHADOW);
    glClearDepth(1), glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(g->shadow);
    light_uniforms(g->shadow, &L, 0);
    f1(g->shadow, "minRadius", (float)(o->radius * L.radius / 720.));
    for (int i = 0; i < 2 && geometry; i++) {
        i1(g->shadow, "mode", i);
        glBindVertexArray(g->vao[i]);
        glDrawArrays(i == 0 ? GL_TRIANGLES : GL_POINTS, 0, g->n[i]);
    }

    /* 2. scene: room, surfaces, particles, lines */
    glBindFramebuffer(GL_FRAMEBUFFER, g->ms_fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1), glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE3), glBindTexture(GL_TEXTURE_2D, g->sh_depth);
    glUseProgram(g->bg);
    camera_uniforms(g->bg, &c, w, h, nearz, farz), light_uniforms(g->bg, &L, 3);
    f1(g->bg, "span", (float)span), f1(g->bg, "floorz", (float)(g->lo[2] - .025 * span)), i1(g->bg, "light", o->light);
    glBindVertexArray(g->empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (geometry) {
        glUseProgram(g->geo);
        camera_uniforms(g->geo, &c, w, h, nearz, farz), light_uniforms(g->geo, &L, 3);
        f1(g->geo, "pointpx", (float)(o->radius * h / 720.));
        v2(g->geo, "range", lo, hi), i1(g->geo, "palette", 0);
        i1(g->geo, "waterDroplets", g->water_surface), i1(g->geo, "coverageTex", 2);
        if (g->water_surface) {
            v3(g->geo, "volumeOrigin", g->origin), v3(g->geo, "volumeExtent", g->extent);
            glActiveTexture(GL_TEXTURE2), glBindTexture(GL_TEXTURE_3D, g->volume_tex[1]);
        }
        for (int i = 0; i < 3; i++) {
            bool solid = cm == SW_CMAP_LAB && !g->volume && o->solid_points; /* as the legend does (labapp.c) */
            glActiveTexture(GL_TEXTURE0);
            palette(g, solid, solid ? SW_CMAP_LAB_SOLID : cm);
            i1(g->geo, "mode", i);
            glBindVertexArray(g->vao[i]);
            glDrawArrays(i == 0 ? GL_TRIANGLES : i == 1 ? GL_POINTS : GL_LINES, 0, g->n[i]);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g->ms_fbo), glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->r_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    /* 3. the computed volume, stopped by the scene's depth */
    glBindFramebuffer(GL_FRAMEBUFFER, g->v_fbo);
    glClearColor(0, 0, 0, 0), glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    if (g->volume) {
        GLuint p = g->vol;
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0), palette(g, 0, cm);
        camera_uniforms(p, &c, w, h, nearz, farz);
        i1(p, "waterSurface", g->water_surface ? 1 : g->iso ? 2 : 0), i1(p, "walls", o->walls && !g->water_surface && !g->iso),
            i1(p, "solidVolume", o->solid_volume && !g->water_surface && !g->iso);
        v3(p, "origin", g->origin), v3(p, "extent", g->extent), v3(p, "cell", g->cell);
        v2(p, "range", lo, hi);
        i1(p, "axis", g->axis), i1(p, "flip", g->flip), f1(p, "fraction", (float)g->fraction);
        f1(p, "cutPosition", (float)(g->axis >= 0 ? g->lo[g->axis] + g->fraction * (g->hi[g->axis] - g->lo[g->axis]) : 0));
        i1(p, "absoluteValue", o->absval), i1(p, "logValue", o->logscale), i1(p, "palette", 0);
        for (int t = 0; t < 2; t++) glActiveTexture(GL_TEXTURE1 + t), glBindTexture(GL_TEXTURE_3D, g->volume_tex[t]);
        glActiveTexture(GL_TEXTURE4), glBindTexture(GL_TEXTURE_2D, g->r_depth);
        i1(p, "fieldTex", 1), i1(p, "materialTex", 2), i1(p, "sceneDepth", 4);
        double ld[3] = {L.dir[0], L.dir[1], L.dir[2]};
        v3(p, "lightDir", ld);
        glBindVertexArray(g->empty_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    /* 4. compose into the app's framebuffer */
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(x, y, w, h);
    glUseProgram(g->comp);
    glActiveTexture(GL_TEXTURE0), glBindTexture(GL_TEXTURE_2D, g->r_color);
    glActiveTexture(GL_TEXTURE1), glBindTexture(GL_TEXTURE_2D, g->r_depth);
    glActiveTexture(GL_TEXTURE2), glBindTexture(GL_TEXTURE_2D, g->v_color);
    i1(g->comp, "aoOff", getenv("NAVIER_NO_AO") != NULL), i1(g->comp, "sceneColor", 0), i1(g->comp, "sceneDepth", 1), i1(g->comp, "volumeColor", 2), i1(g->comp, "light", o->light);
    v2(g->comp, "size", w, h), v2(g->comp, "offset", x, y);
    f1(g->comp, "aoRadius", (float)(.03 * span)), f1(g->comp, "nearz", (float)nearz), f1(g->comp, "farz", (float)farz), f1(g->comp, "focal", (float)(h * .5 / tan(c.fov_deg * M_PI / 360)));
    glBindVertexArray(g->empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(0), glUseProgram(0);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (depth_on) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (blend_on) glEnable(GL_BLEND);
    if (cull_on) glEnable(GL_CULL_FACE);
    return true;
}
