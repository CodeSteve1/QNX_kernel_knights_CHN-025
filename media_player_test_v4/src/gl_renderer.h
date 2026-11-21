// src/gl_renderer.h
#ifndef GL_RENDERER_H
#define GL_RENDERER_H

#include <GLES3/gl3.h>
#include <stdbool.h>

typedef struct {
    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLuint fbo;
    GLuint fbo_tex;
    int off_w;
    int off_h;
} GLRenderer;

bool gl_renderer_init(GLRenderer *r, int offscreen_w, int offscreen_h);
void gl_renderer_shutdown(GLRenderer *r);
void gl_renderer_compute_grid(int count, int *out_cols, int *out_rows);
void gl_renderer_begin_offscreen(GLRenderer *r);
void gl_renderer_end_offscreen(void);
void gl_renderer_draw_quad(const GLRenderer *r, GLuint tex_id, int x, int y, int w, int h);

#endif
