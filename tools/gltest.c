/* gltest - verifies the Cocoa window, OpenGL 4.1 context, CoreText atlas and PNG output */
#include "../src/common.h"
#include "../src/font.h"
#include "../src/glutil.h"
#include "../src/image.h"
#include "../src/platform.h"

static const char *VS = "#version 410 core\n"
                        "layout(location=0) in vec2 p; layout(location=1) in vec2 uv; layout(location=2) in vec3 col;\n"
                        "out vec2 v_uv; out vec3 v_col;\n"
                        "void main(){ v_uv=uv; v_col=col; gl_Position=vec4(p,0,1); }\n";
static const char *FS = "#version 410 core\n"
                        "in vec2 v_uv; in vec3 v_col; uniform sampler2D tex; uniform int mode; out vec4 o;\n"
                        "void main(){ if(mode==0) o=vec4(v_col,1); else { float a=texture(tex,v_uv).r;"
                        " o=vec4(mix(vec3(0.02,0.04,0.08), vec3(0.8,0.95,1.0), a),1);} }\n";

int main(int argc, char **argv) {
    bool headless = false;
    int frames = 120;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    }
    if (!platform_init("NAVIER gltest", 1000, 640, headless)) {
        fprintf(stderr, "platform_init failed\n");
        return 1;
    }
    printf("GL_VERSION  : %s\nGL_RENDERER : %s\nGLSL        : %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER),
           glGetString(GL_SHADING_LANGUAGE_VERSION));
    int w, h, fbw, fbh;
    platform_get_size(&w, &h, &fbw, &fbh);
    printf("size %dx%d pt, framebuffer %dx%d px, scale %.2f\n", w, h, fbw, fbh, platform_backing_scale());

    Font font;
    double t0 = now_seconds();
    if (!font_build(&font, "SFMono-Regular,Menlo-Regular", 30)) {
        printf("font_build failed\n");
        return 1;
    }
    printf("font '%s' px=%.0f atlas %dx%d extra=%d line=%.1f asc=%.1f desc=%.1f  (%.1f ms)\n", font.name, font.px,
           font.atlas_w, font.atlas_h, font.n_extra, font.line_height, font.ascent, font.descent,
           (now_seconds() - t0) * 1e3);
    GLuint tex = gl_texture_2d(font.atlas_w, font.atlas_h, GL_R8, GL_RED, GL_UNSIGNED_BYTE, font.atlas, true);
    GLuint prog = gl_program("gltest", VS, NULL, FS);
    if (!prog) return 1;

    float verts[] = {
        -0.9f, -0.8f, 0, 0, 1, 0.2f, 0.2f,  -0.1f, -0.8f, 0, 0, 0.2f, 1, 0.3f,  -0.5f, 0.8f, 0, 0, 0.2f, 0.4f, 1,
        0.05f, -0.8f, 0, 1, 1, 1, 1,        0.95f, -0.8f, 1, 1, 1, 1, 1,        0.95f, 0.8f, 1, 0, 1, 1, 1,
        0.05f, -0.8f, 0, 1, 1, 1, 1,        0.95f, 0.8f, 1, 0, 1, 1, 1,         0.05f, 0.8f, 0, 0, 1, 1, 1,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    GLTarget tgt;
    if (!gl_target_create(&tgt, fbw, fbh, GL_RGBA8, true, 1)) return 1;
    GLTarget ms;
    bool have_ms = gl_target_create(&ms, fbw, fbh, GL_RGBA16F, true, 4);
    printf("MSAA RGBA16F target: %s\n", have_ms ? "ok" : "failed");

    PlatformEvent ev[64];
    for (int f = 0; f < frames && !platform_should_quit(); f++) {
        int n = platform_poll_events(ev, 64);
        for (int i = 0; i < n; i++) {
            const PlatformEvent *e = &ev[i];
            if (e->type == EV_MOUSE_MOVE) continue;
            printf("event type=%d key=%d cp=%u button=%d clicks=%d pos=(%.0f,%.0f) d=(%.1f,%.1f) mods=%u %s\n", e->type,
                   e->key, e->codepoint, e->button, e->clicks, e->x, e->y, e->dx, e->dy, e->mods, e->path);
        }
        int nw, nh, nfw, nfh;
        platform_get_size(&nw, &nh, &nfw, &nfh);
        if (nfw != tgt.w || nfh != tgt.h) {
            gl_target_destroy(&tgt);
            gl_target_create(&tgt, nfw, nfh, GL_RGBA8, true, 1);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
        glViewport(0, 0, tgt.w, tgt.h);
        glClearColor(0.05f, 0.07f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        glUniform1i(glGetUniformLocation(prog, "mode"), 0);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform1i(glGetUniformLocation(prog, "mode"), 1);
        glDrawArrays(GL_TRIANGLES, 3, 6);
        if (!headless) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, tgt.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, tgt.w, tgt.h, 0, 0, tgt.w, tgt.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        platform_swap();
    }
    gl_check("frame loop");
    if (shot) {
        glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
        uint8_t *px = malloc((size_t)tgt.w * (size_t)tgt.h * 4);
        glReadPixels(0, 0, tgt.w, tgt.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        bool ok = image_write_png(shot, tgt.w, tgt.h, px, true);
        printf("screenshot %s: %s\n", shot, ok ? "ok" : "FAILED");
        free(px);
    }
    platform_shutdown();
    return 0;
}
