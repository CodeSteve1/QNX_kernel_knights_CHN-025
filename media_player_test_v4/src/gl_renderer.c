// src/gl_renderer.c
#include "gl_renderer.h"
#include <stddef.h>

static const char *kVertSrc =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aUV;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *kFragSrc =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = texture(uTex, vUV);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

bool gl_renderer_init(GLRenderer *r, int offscreen_w, int offscreen_h) {
    r->off_w = offscreen_w;
    r->off_h = offscreen_h;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertSrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragSrc);
    r->program = glCreateProgram();
    glAttachShader(r->program, vs);
    glAttachShader(r->program, fs);
    glLinkProgram(r->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    const float quad[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
    };

    glGenVertexArrays(1, &r->vao);
    glGenBuffers(1, &r->vbo);
    glBindVertexArray(r->vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glGenTextures(1, &r->fbo_tex);
    glBindTexture(GL_TEXTURE_2D, r->fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreen_w, offscreen_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void gl_renderer_shutdown(GLRenderer *r) {
    glDeleteFramebuffers(1, &r->fbo);
    glDeleteTextures(1, &r->fbo_tex);
    glDeleteBuffers(1, &r->vbo);
    glDeleteVertexArrays(1, &r->vao);
    glDeleteProgram(r->program);
}

void gl_renderer_compute_grid(int count, int *out_cols, int *out_rows) {
    if (count <= 1)      { *out_cols = 1; *out_rows = 1; }
    else if (count == 2) { *out_cols = 2; *out_rows = 1; }
    else if (count <= 4) { *out_cols = 2; *out_rows = 2; }
    else if (count <= 6) { *out_cols = 3; *out_rows = 2; }
    else if (count <= 9) { *out_cols = 3; *out_rows = 3; }
    else                 { *out_cols = 5; *out_rows = 2; } // 10 tiles
}

void gl_renderer_begin_offscreen(GLRenderer *r) {
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glViewport(0, 0, r->off_w, r->off_h);
}

void gl_renderer_end_offscreen(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_renderer_draw_quad(const GLRenderer *r, GLuint tex_id, int x, int y, int w, int h) {
    if (tex_id == 0) return;
    glViewport(x, y, w, h);
    glUseProgram(r->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glBindVertexArray(r->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}
