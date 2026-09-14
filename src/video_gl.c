/*
Copyright (c) 2020-2026 Rupert Carmichael
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>

#include <epoxy/gl.h>
#include <SDL3/SDL.h>

#define NUMPASSES 2
#define SIZE_GLSLVER 20

#include "jgrf.h"
#include "video.h"
#include "video_gl.h"
#include "settings.h"
#include "osd.h"

static jgrf_gdata_t *gdata = NULL;
static jg_setting_t *settings = NULL;

// SDL Window management
static SDL_Window *window;
static SDL_Cursor *cursor;
static SDL_GLContext glcontext;

// Pointer to the video buffer when allocated by the frontend
static void *videobuf = NULL;

// Pointer to the core's video information
static jg_videoinfo_t *vidinfo = NULL;

// OpenGL related variables
static GLuint vao[NUMPASSES];
static GLuint vbo[NUMPASSES];
static GLuint shaderprog[NUMPASSES];
static GLuint tex[NUMPASSES];
static GLuint texfilter[NUMPASSES];
static GLuint framebuf; // Framebuffer for rendering offscreen
static int glapi = 0;

// Triangle and Texture vertices
static GLfloat vertices[] = {
    -1.0, -1.0, // Vertex 1 (X, Y) Left Bottom
    -1.0, 1.0,  // Vertex 2 (X, Y) Left Top
    1.0, -1.0,  // Vertex 3 (X, Y) Right Bottom
    1.0, 1.0,   // Vertex 4 (X, Y) Right Top

    0.0, 0.0,   // Texture 2 (X, Y) Left Top
    0.0, 1.0,   // Texture 1 (X, Y) Left Bottom
    1.0, 0.0,   // Texture 4 (X, Y) Right Top
    1.0, 1.0,   // Texture 3 (X, Y) Right Bottom
};

// OSD
static GLuint osd_tex = 0;
static GLuint osd_vao = 0;
static GLuint osd_vbo = 0;
static GLuint osd_shader = 0;
static int osd_initialized = 0;

// OSD shader source
static const char *osd_vshader_src =
    "in vec2 vertex;\n"
    "in vec2 vert_tex_coord;\n"
    "out vec2 tex_coord;\n"
    "void main() {\n"
    "    gl_Position = vec4(vertex, 0.0, 1.0);\n"
    "    tex_coord = vert_tex_coord;\n"
    "}\n";

static const char *osd_fshader_src =
    "in vec2 tex_coord;\n"
    "uniform sampler2D osd_texture;\n"
    "out vec4 frag_colour;\n"
    "void main() {\n"
    "    frag_colour = texture(osd_texture, tex_coord);\n"
    "}\n";

// Pixel Format
static unsigned pixfmt_active = JG_PIXFMT_XRGB8888;
static struct _pixfmt {
    GLuint format;
    GLuint format_internal;
    GLuint type;
    size_t size;
} pixfmt;

// Dimensions
static struct _dimensions {
    int ww; int wh;
    float rw; float rh;
    float xo; float yo;
    float dpiscale;
} dimensions;

// Last seen aspect ratio for change detection
static double aspect_prev = 0.0f;

// Create the SDL OpenGL Window
void jgrf_video_gl_create(void) {
    // Set the GL version
    switch (settings[VIDEO_API].val) {
        default: case 0: { // OpenGL - Core Profile
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                SDL_GL_CONTEXT_PROFILE_CORE);
            break;
        }
        case 1: { // OpenGL ES
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                SDL_GL_CONTEXT_PROFILE_ES);
            break;
        }
        case 2: { // OpenGL - Compatibility Profile
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            break;
        }
    }

    // Disable the alpha channel so that the framebuffer is 100% opaque
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);

    // Set window flags
    SDL_WindowFlags windowflags = SDL_WINDOW_OPENGL |
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    // Set up the window
    char title[128];
    gdata = jgrf_gdata_ptr();
    snprintf(title, sizeof(title), "%s", gdata->gamename);

    jgrf_log(JG_LOG_DBG, "SDL Video driver: %s\n",
        SDL_GetCurrentVideoDriver());

    // Set the window dimensions
    if (!strcmp(SDL_GetCurrentVideoDriver(), "kmsdrm")) {
        SDL_DisplayID disp_id = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(disp_id);
        dimensions.ww = dm->w;
        dimensions.wh = dm->h;
    }
    else {
        dimensions.ww =
            (vidinfo->aspect * vidinfo->h * settings[VIDEO_SCALE].val) + 0.5;
        dimensions.wh = (vidinfo->h * settings[VIDEO_SCALE].val) + 0.5;
    }
    dimensions.rw = dimensions.ww;
    dimensions.rh = dimensions.wh;

    jgrf_log(JG_LOG_DBG, "Creating window with dimensions: %d x %d\n",
        dimensions.ww, dimensions.wh);

    // Bring up the window
    window = SDL_CreateWindow(title, dimensions.ww, dimensions.wh,
        windowflags);

    if (!window)
        jgrf_log(JG_LOG_ERR, "Failed to create window: %s\n", SDL_GetError());

    // Store the DPI scale
    int x, y;
    SDL_GetWindowSizeInPixels(window, &x, &y);
    dimensions.dpiscale = (float)x/dimensions.ww;

    jgrf_video_icon_load(window);

    // Set the GL context
    glcontext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glcontext);
    SDL_GL_SetSwapInterval(!bmark); // Vsync off in Benchmark mode

    if (!glcontext)
        jgrf_log(JG_LOG_WRN, "Failed to create glcontext: %s\n",
            SDL_GetError());

    // Do post window creation OpenGL setup
    if (settings[VIDEO_API].val > 1)
        jgrf_video_gl_setup_compat();
    else
        jgrf_video_gl_setup();

    jgrf_log(JG_LOG_INF, "Video: OpenGL %s\n", glGetString(GL_VERSION));

    SDL_HideCursor();

    // Set fullscreen if required
    if (settings[VIDEO_FULLSCREEN].val)
        SDL_SetWindowFullscreen(window, true);
}

// Initialize video buffer
int jgrf_video_gl_init(void) {
    // Grab settings and global data pointers
    settings = jgrf_settings_ptr();
    gdata = jgrf_gdata_ptr();

    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL)) {
        // Address of allocated memory owned by frontend but passed to core
        videobuf = (void*)calloc(vidinfo->wmax * vidinfo->hmax, pixfmt.size);
        vidinfo->buf = videobuf;
    }

    if (gdata->hints & JG_HINT_VIDEO_PRESCALED)
        settings[VIDEO_SCALE].val = 1;

    glapi = settings[VIDEO_API].val;

    jgrf_osd_init();

    return 1;
}

// Flip the image so that it can be written out with proper orientation
static inline void jgrf_video_gl_ssflip(uint8_t *pixels,
    int width, int height, int bytes) {
    // Flip the pixels
    size_t rowsize = width * bytes;
    uint8_t *row = (uint8_t*)calloc(rowsize, sizeof(uint8_t));
    uint8_t *low = pixels;
    uint8_t *high = &pixels[(height - 1) * rowsize];

    for (; low < high; low += rowsize, high -= rowsize) {
        memcpy(row, low, rowsize);
        memcpy(low, high, rowsize);
        memcpy(high, row, rowsize);
    }
    free(row);
}

// Dump the pixels rendered to the default framebuffer
void *jgrf_video_gl_get_pixels(int *rw, int *rh) {
    uint8_t *pixels = (uint8_t*)calloc(dimensions.rw * dimensions.rh,
        sizeof(uint32_t));

    // Clear any OSD content before grabbing the framebuffer
    for (int i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i)
        jgrf_osd_text_clear(i);
    jgrf_osd_shade_set(0);
    jgrf_osd_fx_clear();
    if (glapi > 1)
        jgrf_video_gl_render_compat(0);
    else
        jgrf_video_gl_render(0);

    // Read the pixels and flip them vertically
    glReadPixels(dimensions.xo, dimensions.yo, dimensions.rw, dimensions.rh,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    jgrf_video_gl_ssflip(pixels, dimensions.rw, dimensions.rh,
        sizeof(uint32_t));
    *rw = dimensions.rw;
    *rh = dimensions.rh;
    return pixels;
}

// Deinitialize OpenGL Video
void jgrf_video_gl_deinit(void) {
    if (osd_tex) glDeleteTextures(1, &osd_tex);
    if (osd_vbo) glDeleteBuffers(1, &osd_vbo);
    if (osd_vao) glDeleteVertexArrays(1, &osd_vao);
    if (osd_shader) glDeleteProgram(osd_shader);
    osd_tex = osd_vbo = osd_vao = osd_shader = 0;
    osd_initialized = 0;
    jgrf_osd_deinit();

    if (framebuf) glDeleteFramebuffers(1, &framebuf);

    for (int i = 0; i < NUMPASSES; ++i) {
        if (shaderprog[i]) glDeleteProgram(shaderprog[i]);
        if (tex[i]) glDeleteTextures(1, &tex[i]);
        if (vao[i]) glDeleteVertexArrays(1, &vao[i]);
        if (vbo[i]) glDeleteBuffers(1, &vbo[i]);
    }

    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL))
        if (videobuf) free(videobuf);

    if (cursor) SDL_DestroyCursor(cursor);
    if (glcontext) SDL_GL_DestroyContext(glcontext);
    SDL_DestroyWindow(window);
}

// Toggle between fullscreen and windowed
void jgrf_video_gl_fullscreen(void) {
    settings[VIDEO_FULLSCREEN].val ^= 1;
    SDL_SetWindowFullscreen(window,
        settings[VIDEO_FULLSCREEN].val ? true : false);

    // Fullscreen toggle does not trigger SDL_WINDOWEVENT_RESIZED
    jgrf_video_gl_resize();
}

// Set the GL Pixel Format
static void jgrf_video_gl_refresh_pixfmt(void) {
    pixfmt_active = vidinfo->pixfmt;
    switch (vidinfo->pixfmt) {
        case JG_PIXFMT_XRGB8888: {
            pixfmt.format = GL_BGRA;
            pixfmt.format_internal = GL_RGBA;
            pixfmt.type = GL_UNSIGNED_BYTE;
            pixfmt.size = sizeof(uint32_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: GL_UNSIGNED_BYTE\n");
            break;
        }
        case JG_PIXFMT_XBGR8888: {
            pixfmt.format = GL_RGBA;
            pixfmt.format_internal = GL_RGBA;
            pixfmt.type = GL_UNSIGNED_BYTE;
            pixfmt.size = sizeof(uint32_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: GL_UNSIGNED_BYTE\n");
            break;
        }
        case JG_PIXFMT_RGBX5551: {
            pixfmt.format = GL_RGBA;
            pixfmt.format_internal = GL_RGBA;
            pixfmt.type = GL_UNSIGNED_SHORT_5_5_5_1;
            pixfmt.size = sizeof(uint16_t);
            jgrf_log(JG_LOG_DBG,
                "Pixel format: GL_UNSIGNED_SHORT_5_5_5_1\n");
            break;
        }
        case JG_PIXFMT_RGB565: {
            pixfmt.format = GL_RGB;
            pixfmt.format_internal = GL_RGB;
            pixfmt.type = GL_UNSIGNED_SHORT_5_6_5;
            pixfmt.size = sizeof(uint16_t);
            jgrf_log(JG_LOG_DBG,
                "Pixel format: GL_UNSIGNED_SHORT_5_6_5\n");
            break;
        }
        default: {
            jgrf_log(JG_LOG_ERR, "Unknown pixel format, exiting...\n");
            break;
        }
    }
}

// Refresh any video settings that may have changed
static void jgrf_video_gl_refresh(void) {
    // Recalculate viewport if the aspect ratio changed
    if (vidinfo->aspect != aspect_prev) {
        aspect_prev = vidinfo->aspect;
        jgrf_video_gl_resize();
    }

    float top = (float)vidinfo->y / vidinfo->hmax;
    float bottom = 1.0 + top -
        ((vidinfo->hmax - (float)vidinfo->h) / vidinfo->hmax);
    float left = (float)vidinfo->x / vidinfo->wmax;
    float right = 1.0 + left -
        ((vidinfo->wmax -(float)vidinfo->w) / vidinfo->wmax);

    if (pixfmt_active != vidinfo->pixfmt)
        jgrf_video_gl_refresh_pixfmt();

    // Check if any vertices have changed since last time
    if (vertices[9] != top || vertices[11] != bottom
        || vertices[8] != left || vertices[12] != right) {
        vertices[9] = vertices[13] = top;
        vertices[11] = vertices[15] = bottom;
        vertices[8] = vertices[10] = left;
        vertices[12] = vertices[14] = right;
    }
    else { // If nothing changed, return
        return;
    }

    // Bind the VAO/VBO for the offscreen texture, update with new vertex data
    glBindVertexArray(vao[0]);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Resize the offscreen texture
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, pixfmt.format_internal,
        vidinfo->wmax, vidinfo->hmax, 0, pixfmt.format, pixfmt.type, NULL);

    // Set row length
    glUseProgram(shaderprog[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, vidinfo->p);

    // Resize the output texture
    glUseProgram(shaderprog[1]);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, pixfmt.format_internal,
        vidinfo->w, vidinfo->h, 0, pixfmt.format, pixfmt.type, NULL);

    // Update uniforms for post-processing
    glUniform4f(glGetUniformLocation(shaderprog[1], "sourceSize"),
        (float)vidinfo->w, (float)vidinfo->h,
        1.0/(float)vidinfo->w, 1.0/(float)vidinfo->h);
    glUniform4f(glGetUniformLocation(shaderprog[1], "targetSize"),
        dimensions.rw, dimensions.rh,
        1.0/dimensions.rw, 1.0/dimensions.rh);
}

static void jgrf_video_gl_osd_setup(void) {
    /* Lazy initialisation of OSD GL resources. Deferred until the first frame
       because shader compilation needs the GL context fully ready.
    */
    if (osd_initialized) return;

    /* Allocate the OSD texture at the canvas's fixed size. Filtering is
       GL_NEAREST so integer scaling produces crisp bitmap text.
    */
    glGenTextures(1, &osd_tex);
    glBindTexture(GL_TEXTURE_2D, osd_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, JGRF_OSD_W, JGRF_OSD_H, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* For Core Profile/GLES, set up a small VAO/VBO/shader. For Compatibility
       Profile, use immediate mode in the render function and skip these.
    */
    if (glapi < 2) {
        const char *ver = (glapi == 1) ?
            "#version 300 es\nprecision highp float;\n" :
            "#version 140\n";

        char vsrc[512], fsrc[512];
        snprintf(vsrc, sizeof(vsrc), "%s%s", ver, osd_vshader_src);
        snprintf(fsrc, sizeof(fsrc), "%s%s", ver, osd_fshader_src);

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        const char *vp = vsrc;
        glShaderSource(vs, 1, &vp, NULL);
        glCompileShader(vs);
        GLint ok = 0;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024]; GLsizei len = 0;
            glGetShaderInfoLog(vs, sizeof(log), &len, log);
            jgrf_log(JG_LOG_WRN, "OSD vshader compile: %.*s\n", (int)len, log);
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fp = fsrc;
        glShaderSource(fs, 1, &fp, NULL);
        glCompileShader(fs);
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024]; GLsizei len = 0;
            glGetShaderInfoLog(fs, sizeof(log), &len, log);
            jgrf_log(JG_LOG_WRN, "OSD fshader compile: %.*s\n", (int)len, log);
        }

        osd_shader = glCreateProgram();
        glAttachShader(osd_shader, vs);
        glAttachShader(osd_shader, fs);
        glBindAttribLocation(osd_shader, 0, "vertex");
        glBindAttribLocation(osd_shader, 1, "vert_tex_coord");
        glLinkProgram(osd_shader);
        glGetProgramiv(osd_shader, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024]; GLsizei len = 0;
            glGetProgramInfoLog(osd_shader, sizeof(log), &len, log);
            jgrf_log(JG_LOG_WRN, "OSD shader link: %.*s\n", (int)len, log);
        }
        glDeleteShader(vs);
        glDeleteShader(fs);

        /* Bind the sampler uniform to texture unit 0 explicitly. Most drivers
           default to 0, but some don't.
        */
        glUseProgram(osd_shader);
        GLint sloc = glGetUniformLocation(osd_shader, "osd_texture");
        if (sloc >= 0) glUniform1i(sloc, 0);

        glGenVertexArrays(1, &osd_vao);
        glBindVertexArray(osd_vao);

        glGenBuffers(1, &osd_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, osd_vbo);
        // 4 vertices, each (x, y, u, v). Filled in per-frame.
        glBufferData(GL_ARRAY_BUFFER, 4 * 4 * sizeof(GLfloat), NULL,
            GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
            (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
            (void*)(2 * sizeof(GLfloat)));
    }

    osd_initialized = 1;
}

// Compute the OSD's destination rectangle (in window pixels, post-shader)
static void jgrf_video_gl_osd_compute_rects(float *u0, float *v0,
    float *u1, float *v1, int *vis_w_out, int *vis_h_out) {

    float rw = dimensions.rw;
    float rh = dimensions.rh;

    /* OSD scale ticks up every time the render area crosses another
       256x224 boundary on both axes, rounded to the nearest step rather
       than floored - so a window just shy of the next boundary still
       moves up.
    */
    int scale_x = ((int)rw + 128) / 256;
    int scale_y = ((int)rh + 112) / 224;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;

    /* Visible region in canvas pixels: how much of the canvas the OSD
       module fills, and what UV span the OSD quad samples. The render
       area is the viewport itself (set by the caller), so the quad
       always fills it exactly - we just need enough canvas to cover.
    */
    int vw = ((int)rw + scale - 1) / scale;
    if (vw > JGRF_OSD_W) vw = JGRF_OSD_W;
    int vh = ((int)rh + scale - 1) / scale;
    if (vh > JGRF_OSD_H) vh = JGRF_OSD_H;

    *u0 = 0.0f;
    *v0 = 0.0f;
    *u1 = rw / ((float)scale * (float)JGRF_OSD_W);
    *v1 = rh / ((float)scale * (float)JGRF_OSD_H);

    *vis_w_out = vw;
    *vis_h_out = vh;
}

static void jgrf_video_gl_osd_draw_modern(void) {
    int rw = (int)dimensions.rw;
    int rh = (int)dimensions.rh;
    if (rw < 1 || rh < 1) return;

    int vw, vh;
    float u0, v0, u1, v1;
    jgrf_video_gl_osd_compute_rects(&u0, &v0, &u1, &v1, &vw, &vh);

    jgrf_osd_set_visible(vw, vh);
    jgrf_osd_step();

    int active = 0, dirty = 0;
    const void *pixels = jgrf_osd_composite(&active, &dirty);
    if (!active) return;

    jgrf_video_gl_osd_setup();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, osd_tex);
    if (dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
            JGRF_OSD_W, JGRF_OSD_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    glViewport(dimensions.xo, dimensions.yo, dimensions.rw, dimensions.rh);

    GLfloat verts[] = {
        -1.0f, -1.0f, u0, v1,
        -1.0f,  1.0f, u0, v0,
         1.0f, -1.0f, u1, v1,
         1.0f,  1.0f, u1, v0
    };

    glUseProgram(osd_shader);
    glBindVertexArray(osd_vao);
    glBindBuffer(GL_ARRAY_BUFFER, osd_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

static void jgrf_video_gl_osd_draw_compat(void) {
    int rw = (int)dimensions.rw;
    int rh = (int)dimensions.rh;
    if (rw < 1 || rh < 1) return;

    int vw, vh;
    float u0, v0, u1, v1;
    jgrf_video_gl_osd_compute_rects(&u0, &v0, &u1, &v1, &vw, &vh);

    jgrf_osd_set_visible(vw, vh);
    jgrf_osd_step();

    int active = 0, dirty = 0;
    const void *pixels = jgrf_osd_composite(&active, &dirty);
    if (!active)
        return;

    jgrf_video_gl_osd_setup();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, osd_tex);
    if (dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); /* tightly packed */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
            JGRF_OSD_W, JGRF_OSD_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    glViewport(dimensions.xo, dimensions.yo, dimensions.rw, dimensions.rh);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    /* The compat setup forces texture alpha to a constant 1.0 via a
       GL_COMBINE env to keep the game framebuffer fully opaque. That
       breaks the OSD: every pixel of the OSD quad would become opaque,
       blacking out the screen wherever the OSD is empty. Switch to plain
       GL_REPLACE for the OSD draw so the texture's actual RGBA reaches
       the blender, then restore the COMBINE setup before returning.
    */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glBegin(GL_QUADS);
        glTexCoord2f(u0, v1); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(u1, v1); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(u1, v0); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(u0, v0); glVertex2f(-1.0f,  1.0f);
    glEnd();

    glDisable(GL_BLEND);

    /* Restore the alpha-forcing combine setup so the next game-quad draw
       still produces an opaque framebuffer. Mirrors the original setup
       in jgrf_video_gl_setup_compat. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_CONSTANT);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

// Render the scene
void jgrf_video_gl_render(int render) {
    jgrf_video_gl_refresh(); // Check for changes

    // Viewport set to size of the input pixel array
    glViewport(0, 0, vidinfo->w, vidinfo->h);

    // Make sure first pass shader program is active
    glUseProgram(shaderprog[0]);

    // Bind user-created framebuffer and draw scene onto it
    glBindFramebuffer(GL_FRAMEBUFFER, framebuf);
    glBindVertexArray(vao[0]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);

    // Render if there is new pixel data, do Black Frame Insertion otherwise
    if (render) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, vidinfo->p);
        glTexSubImage2D(GL_TEXTURE_2D,
                0,
                0, // xoffset
                0, // yoffset
                vidinfo->w + vidinfo->x, // width
                vidinfo->h + vidinfo->y, // height
                pixfmt.format, // format
                pixfmt.type, // type
            vidinfo->buf);
    }

    // Clear the screen to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw a rectangle from the 2 triangles
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Now deal with the actual image to be output
    // Viewport adjusted for output
    glViewport(dimensions.xo, dimensions.yo, dimensions.rw, dimensions.rh);

    // Make sure second pass shader program is active
    glUseProgram(shaderprog[1]);

    // Bind default framebuffer and draw contents of user framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(vao[1]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[1]);

    // Clear the screen to black again
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw framebuffer contents
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // OSD pass: composite layers, upload texture, draw alpha-blended quad
    jgrf_video_gl_osd_draw_modern();
}

// Render the scene
void jgrf_video_gl_render_compat(int render) {
    jgrf_video_gl_refresh(); // Check for changes

    // Viewport set to size of the output
    glViewport(dimensions.xo, dimensions.yo, dimensions.rw, dimensions.rh);

    // Clear the screen to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);

    // Render if there is new pixel data, do Black Frame Insertion otherwise
    if (render) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, vidinfo->p);
        glTexSubImage2D(GL_TEXTURE_2D,
                0,
                0, // xoffset
                0, // yoffset
                vidinfo->w + vidinfo->x, // width
                vidinfo->h + vidinfo->y, // height
                pixfmt.format, // format
                pixfmt.type, // type
            vidinfo->buf);
    }

    glEnable(GL_TEXTURE_2D);

    glBegin(GL_QUADS);
        glTexCoord2f(vertices[10], vertices[11]);
        glVertex2f(vertices[0], vertices[1]); // Bottom Left

        glTexCoord2f(vertices[8], vertices[9]);
        glVertex2f(vertices[2], vertices[3]); // Top Left

        glTexCoord2f(vertices[12], vertices[13]);
        glVertex2f(vertices[6], vertices[7]); // Top Right

        glTexCoord2f(vertices[14], vertices[15]);
        glVertex2f(vertices[4], vertices[5]); // Bottom Right
    glEnd();

    // OSD pass: draw composited canvas as alpha-blended quad over the game
    jgrf_video_gl_osd_draw_compat();
}

// Handle viewport resizing
void jgrf_video_gl_resize(void) {
    SDL_GetWindowSizeInPixels(window, &dimensions.ww, &dimensions.wh);
    dimensions.rw = dimensions.ww;
    dimensions.rh = dimensions.wh;

    // Check which dimension to optimize
    if (dimensions.rh * vidinfo->aspect > dimensions.rw)
        dimensions.rh = dimensions.rw / vidinfo->aspect + 0.5;
    else if (dimensions.rw / vidinfo->aspect > dimensions.rh)
        dimensions.rw = dimensions.rh * vidinfo->aspect + 0.5;

    // Store X and Y offsets
    dimensions.xo = (dimensions.ww - dimensions.rw) / 2;
    dimensions.yo = (dimensions.wh - dimensions.rh) / 2;

    // Update the targetSize uniform
    glUniform4f(glGetUniformLocation(shaderprog[1], "targetSize"),
        dimensions.rw, dimensions.rh,
        1.0/dimensions.rw, 1.0/dimensions.rh);

    // Get current display mode
    SDL_DisplayID displayid = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(displayid);

    // Set the base fps for use in the main loop
    if (dm)
        jgrf_set_screenfps((int)(dm->refresh_rate + 0.5));
}

// Retrieve scale parameters for pointing device input
void jgrf_video_gl_get_scale_params(float *xscale, float *yscale,
    float *xo, float *yo) {
    *xscale = dimensions.rw /
        (vidinfo->aspect * vidinfo->h) / dimensions.dpiscale;
    *yscale = dimensions.rh / vidinfo->h / dimensions.dpiscale;
    *xo = dimensions.xo / dimensions.dpiscale;
    *yo = dimensions.yo / dimensions.dpiscale;
}

// Retrieve video information
jg_videoinfo_t* jgrf_video_gl_get_info(void) {
    return vidinfo;
}

// Set the cursor
void jgrf_video_gl_set_cursor(int ctype) {
    cursor = SDL_CreateSystemCursor(ctype);
    SDL_ShowCursor();
    SDL_SetCursor(cursor);
}

// Pass a pointer to the video info held in  the core into the frontend
void jgrf_video_gl_set_info(jg_videoinfo_t *ptr) {
    vidinfo = ptr;

    // Also set the GL pixel format at this time
    jgrf_video_gl_refresh_pixfmt();
}

// Load a shader source file into memory
static const GLchar* jgrf_video_gl_shader_load(const char *filename) {
    FILE *file = fopen(filename, "rb");

    if (!file)
        jgrf_log(JG_LOG_ERR, "Could not open shader file, exiting...\n");

    // Get the size of the shader source file
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    rewind(file);

    // Allocate memory to store the shader source including version string
    GLchar *src = (GLchar*)calloc(size + SIZE_GLSLVER, sizeof(GLchar));
    if (!src)
        jgrf_log(JG_LOG_ERR, "Could not allocate memory, exiting...\n");

    // Allocate memory for the shader source without version string
    GLchar *shader = (GLchar*)calloc(size + 1, sizeof(GLchar));

    // Write version string into the buffer for the full shader source
    snprintf(src, SIZE_GLSLVER, "%s", settings[VIDEO_API].val ?
        "#version 300 es\n" : "#version 140\n");

    if (!shader || !fread(shader, size, sizeof(GLchar), file)) {
        free(src);
        fclose(file);
        jgrf_log(JG_LOG_ERR, "Could not open shader file, exiting...\n");
        return NULL;
    }

    // Close file handle after reading
    fclose(file);

    // Append shader source to version string
    src = strncat(src, shader, size + SIZE_GLSLVER);

    // Free the shader source without version string
    free(shader);

    return src;
}

// Create a shader program from a vertex shader and a fragment shader
static GLuint jgrf_video_gl_prog_create(const char *vs, const char *fs) {
    char vspath[192];
    char fspath[192];
    struct stat fbuf; // First find what path to use. Check local first.

    snprintf(vspath, sizeof(vspath), "%s%cshaders%cdefault.vs",
        gdata->binpath, SEP, SEP);

    if (stat(vspath, &fbuf) == 0) { // Found it locally
        snprintf(vspath, sizeof(vspath), "%s%cshaders%c%s",
            gdata->binpath, SEP, SEP, vs);
        snprintf(fspath, sizeof(fspath), "%s%cshaders%c%s",
            gdata->binpath, SEP, SEP, fs);
    }
#ifdef JGRF_STATIC

#if defined(DATADIR)
    else { // Use the system-wide path
        snprintf(vspath, sizeof(vspath), "%s%cjollygood%c%s%cshaders%c%s",
            DATADIR, SEP, SEP, jg_get_coreinfo("")->name, SEP, SEP, vs);
        snprintf(fspath, sizeof(fspath), "%s%cjollygood%c%s%cshaders%c%s",
            DATADIR, SEP, SEP, jg_get_coreinfo("")->name, SEP, SEP, fs);
    }
#endif

#else

#if defined(DATADIR)
    else { // Use the system-wide path
        snprintf(vspath, sizeof(vspath), "%s%cjollygood%cjgrf%cshaders%c%s",
            DATADIR, SEP, SEP, SEP, SEP, vs);
        snprintf(fspath, sizeof(fspath), "%s%cjollygood%cjgrf%cshaders%c%s",
            DATADIR, SEP, SEP, SEP, SEP, fs);
    }
#endif

#endif
    const GLchar *vsrc = jgrf_video_gl_shader_load(vspath);
    const GLchar *fsrc = jgrf_video_gl_shader_load(fspath);
    GLint err;

    // Create and compile the vertex shader
    GLuint vshader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vshader, 1, &vsrc, NULL);
    glCompileShader(vshader);

    // Test if the shader compiled
    glGetShaderiv(vshader, GL_COMPILE_STATUS, &err);
    if (err == GL_FALSE) {
        char shaderlog[1024];
        glGetShaderInfoLog(vshader, 1024, NULL, shaderlog);
        jgrf_log(JG_LOG_WRN, "Vertex shader: %s", shaderlog);
    }

    // Create and compile the fragment shader
    GLuint fshader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fshader, 1, &fsrc, NULL);
    glCompileShader(fshader);

    // Test if the fragment shader compiled
    glGetShaderiv(fshader, GL_COMPILE_STATUS, &err);
    if (err == GL_FALSE) {
        char shaderlog[1024];
        glGetShaderInfoLog(fshader, 1024, NULL, shaderlog);
        jgrf_log(JG_LOG_WRN, "Fragment shader: %s", shaderlog);
    }

    // Free the allocated memory for shader sources
    free((GLchar*)vsrc);
    free((GLchar*)fsrc);

    // Create the shader program
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vshader);
    glAttachShader(prog, fshader);
    glLinkProgram(prog);

    // Clean up fragment and vertex shaders
    glDeleteShader(vshader);
    glDeleteShader(fshader);

    // Return the successfully linked shader program
    return prog;
}

static void jgrf_video_gl_shader_setup(void) {
    for (int i = 0; i < NUMPASSES; ++i) {
        if (shaderprog[i]) glDeleteProgram(shaderprog[i]);
        texfilter[i] = GL_NEAREST;
    }

    /* Bind vao[0] so the attribute pointer setup below applies to it.
       At init time vao[0] happens to be bound by the surrounding setup
       code, but rehash() can be called from anywhere - including
       immediately after an OSD draw, when osd_vao is the bound VAO.
       Without this bind, the OSD VAO ends up pointing at the game's
       vertex buffer and the OSD silently breaks.
    */
    glBindVertexArray(vao[0]);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);

    // Create the shader program for the first pass (clipping)
    shaderprog[0] =
        jgrf_video_gl_prog_create("default.vs", "default.fs");

    GLint posattrib = glGetAttribLocation(shaderprog[0], "position");
    glEnableVertexAttribArray(posattrib);
    glVertexAttribPointer(posattrib, 2, GL_FLOAT, GL_FALSE, 0, 0);

    GLint texattrib = glGetAttribLocation(shaderprog[0], "vtxCoord");
    glEnableVertexAttribArray(texattrib);
    glVertexAttribPointer(texattrib, 2, GL_FLOAT, GL_FALSE,
        0, (void*)(8 * sizeof(GLfloat)));

    // Set up uniform for input texture
    glUseProgram(shaderprog[0]);
    glUniform1i(glGetUniformLocation(shaderprog[0], "source"), 0);

    switch (settings[VIDEO_SHADER].val) {
        default: case 0: { // Nearest Neighbour
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "default.fs");
            break;
        }
        case 1: { // Linear
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "default.fs");
            texfilter[1] = GL_LINEAR;
            break;
        }
        case 2: { // Sharp Bilinear
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "sharp-bilinear.fs");
            texfilter[0] = GL_LINEAR;
            texfilter[1] = GL_LINEAR;
            break;
        }
        case 3: { // AANN
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "aann.fs");
            break;
        }
        case 4: { // CRT-Yee64
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "crt-yee64.fs");
            break;
        }
        case 5: { // CRTea
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "crtea.fs");
            break;
        }
        case 6: { // LCD
            shaderprog[1] =
                jgrf_video_gl_prog_create("default.vs", "lcd.fs");
            break;
        }
    }

    // Set texture parameters for input texture
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texfilter[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texfilter[0]);

    // Bind vertex array and specify layout for second pass
    glBindVertexArray(vao[1]);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);

    GLint posattrib_out = glGetAttribLocation(shaderprog[1], "position");
    glEnableVertexAttribArray(posattrib_out);
    glVertexAttribPointer(posattrib_out, 2, GL_FLOAT, GL_FALSE, 0, 0);

    GLint texattrib_out = glGetAttribLocation(shaderprog[1], "vtxCoord");
    glEnableVertexAttribArray(texattrib_out);
    glVertexAttribPointer(texattrib_out, 2, GL_FLOAT, GL_FALSE,
        0, (void*)(8 * sizeof(GLfloat)));

    // Set up uniforms for post-processing texture
    glUseProgram(shaderprog[1]);

    glUniform1i(glGetUniformLocation(shaderprog[1], "source"), 0);
    glUniform4f(glGetUniformLocation(shaderprog[1], "sourceSize"),
        (float)vidinfo->w, (float)vidinfo->h,
        1.0/(float)vidinfo->w, 1.0/(float)vidinfo->h);
    glUniform4f(glGetUniformLocation(shaderprog[1], "targetSize"),
        dimensions.rw, dimensions.rh,
        1.0/dimensions.rw, 1.0/dimensions.rh);

        // Settings for CRTea
    int masktype = 0, maskstr = 0, scanstr = 0, sharpness = 0,
        curve = settings[VIDEO_CRTEA_CURVE].val,
        corner = settings[VIDEO_CRTEA_CORNER].val,
        tcurve = settings[VIDEO_CRTEA_TCURVE].val;

    switch (settings[VIDEO_CRTEA_MODE].val) {
        default: case 0: { // Scanlines
            masktype = 0; maskstr = 0; scanstr = 6; sharpness = 3;
            break;
        }
        case 1: { // Aperture Grille Lite
            masktype = 1; maskstr = 5; scanstr = 6; sharpness = 7;
            break;
        }
        case 2: { // Aperture Grille
            masktype = 2; maskstr = 5; scanstr = 6; sharpness = 7;
            break;
        }
        case 3: { // Shadow Mask
            masktype = 3; maskstr = 4; scanstr = 6; sharpness = 3;
            break;
        }
        case 4: { // Custom
            masktype = settings[VIDEO_CRTEA_MASKTYPE].val;
            maskstr = settings[VIDEO_CRTEA_MASKSTR].val;
            scanstr = settings[VIDEO_CRTEA_SCANSTR].val;
            sharpness = settings[VIDEO_CRTEA_SHARPNESS].val;
            break;
        }
    }
    glUniform1i(glGetUniformLocation(shaderprog[1], "masktype"),
        masktype);
    glUniform1f(glGetUniformLocation(shaderprog[1], "maskstr"),
        maskstr / 10.0);
    glUniform1f(glGetUniformLocation(shaderprog[1], "scanstr"),
        scanstr / 10.0);
    glUniform1f(glGetUniformLocation(shaderprog[1], "sharpness"),
        (float)sharpness);
    glUniform1f(glGetUniformLocation(shaderprog[1], "curve"),
        curve / 100.0);
    glUniform1f(glGetUniformLocation(shaderprog[1], "corner"),
        corner ? (float)corner : -3.0);
    glUniform1f(glGetUniformLocation(shaderprog[1], "tcurve"),
        tcurve / 10.0);

    // Set parameters for output texture
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texfilter[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texfilter[1]);
}

// Set up OpenGL
void jgrf_video_gl_setup(void) {
    // Create Vertex Array Objects
    glGenVertexArrays(1, &vao[0]);
    glGenVertexArrays(1, &vao[1]);

    // Create Vertex Buffer Objects
    glGenBuffers(1, &vbo[0]);
    glGenBuffers(1, &vbo[1]);

    // Bind buffers for vertex buffer objects
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices),
        vertices, GL_STATIC_DRAW);

    GLfloat vertices_out[] = {
        -1.0, -1.0, // Vertex 1 (X, Y) Left Bottom
        -1.0, 1.0,  // Vertex 2 (X, Y) Left Top
        1.0, -1.0,  // Vertex 3 (X, Y) Right Bottom
        1.0, 1.0,   // Vertex 4 (X, Y) Right Top
        0.0, 1.0,   // Texture 1 (X, Y) Left Bottom
        0.0, 0.0,   // Texture 2 (X, Y) Left Top
        1.0, 1.0,   // Texture 3 (X, Y) Right Bottom
        1.0, 0.0,   // Texture 4 (X, Y) Right Top
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices_out),
        vertices_out, GL_STATIC_DRAW);

    // Bind vertex array and specify layout for first pass
    glBindVertexArray(vao[0]);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);

    // Generate texture for raw game output
    glGenTextures(1, &tex[0]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);

    // The full sized source image before any clipping
    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL)) {
        glTexImage2D(GL_TEXTURE_2D, 0, pixfmt.format_internal,
            vidinfo->wmax, vidinfo->hmax, 0, pixfmt.format, pixfmt.type,
            vidinfo->buf);
    }

    // Create framebuffer
    glGenFramebuffers(1, &framebuf);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuf);

    // Create texture to hold colour buffer
    glGenTextures(1, &tex[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[1]);

    // The framebuffer texture that is being rendered to offscreen, after clip
    glTexImage2D(GL_TEXTURE_2D, 0, pixfmt.format_internal,
        vidinfo->w, vidinfo->h, 0, pixfmt.format, pixfmt.type, NULL);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        tex[1], 0);

    jgrf_video_gl_shader_setup();

    jgrf_video_gl_resize();
    jgrf_video_gl_refresh();
}

// Set up OpenGL - Compatibility Profile
void jgrf_video_gl_setup_compat(void) {
    switch (settings[VIDEO_SHADER].val) {
        case 0: { // Nearest Neighbour
            texfilter[0] = GL_NEAREST;
            break;
        }
        case 1: { // Linear
            texfilter[0] = GL_LINEAR;
            break;
        }
        default: {
            texfilter[0] = GL_LINEAR;
            break;
        }
    }

    // Generate texture for raw game output
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &tex[0]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex[0]);

    // The full sized source image before any clipping
    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL)) {
        glTexImage2D(GL_TEXTURE_2D, 0, pixfmt.format_internal,
            vidinfo->wmax, vidinfo->hmax, 0, pixfmt.format, pixfmt.type,
            vidinfo->buf);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texfilter[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texfilter[0]);

    // Force the alpha channel to be 100% opaque
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_CONSTANT);
    GLfloat constcolor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constcolor);

    jgrf_video_gl_resize();
    jgrf_video_gl_refresh();

    jgrf_log(JG_LOG_WRN,
        "OpenGL Compatibility Profile supports basic functionality only - no "
        "post-processing shaders.\n"
    );
}

// Swap Buffers
void jgrf_video_gl_swapbuffers(void) {
    SDL_GL_SwapWindow(window);
}

// Set a text message to be output for a number of frames
void jgrf_video_gl_text(int index, int frames, const char *msg) {
    int anchor;
    int x = 0, y = 0;
    int f = frames;

    switch (index) {
        case JGRF_OSD_TEXT_FRONTEND:
            anchor = JGRF_OSD_ANCHOR_BL;
            x = 0; y = 0;
            break;
        case JGRF_OSD_TEXT_CORE:
            anchor = JGRF_OSD_ANCHOR_BR;
            x = 0; y = 0;
            break;
        case JGRF_OSD_TEXT_MENU:
            anchor = JGRF_OSD_ANCHOR_TL;
            x = 8; y = 8;
            /* Menu slot is sticky: any positive frames count means "keep
               showing until cleared". 0 clears it.
            */
            f = (frames > 0) ? -1 : 0;
            break;
        case JGRF_OSD_TEXT_AUX:
        default:
            anchor = JGRF_OSD_ANCHOR_TR;
            x = 0; y = 0;
            break;
    }

    if (f == 0 || !msg || msg[0] == '\0') {
        jgrf_osd_text_clear(index);
        return;
    }
    jgrf_osd_text(index, f, anchor, x, y, 0, msg);
}

void jgrf_video_gl_rehash(void) {
    // Avoid applying shaders under Compatibility Profile
    if (glapi < 2)
        jgrf_video_gl_shader_setup();

    SDL_SetWindowFullscreen(window, settings[VIDEO_FULLSCREEN].val ?
        true : false);
    jgrf_video_gl_resize();

    jgrf_osd_rehash();
}
