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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "lodepng.h"

#include "jgrf.h"
#include "video.h"
#include "video_gl.h"
#ifdef JGRF_VULKAN
#include "video_vk.h"
#endif
#include "settings.h"

void (*jgrf_video_create)(void);
int (*jgrf_video_init)(void);
void (*jgrf_video_deinit)(void);
void (*jgrf_video_fullscreen)(void);
void (*jgrf_video_render)(int);
void (*jgrf_video_resize)(void);
void (*jgrf_video_get_scale_params)(float*, float*, float*, float*);
void (*jgrf_video_set_cursor)(int);
jg_videoinfo_t* (*jgrf_video_get_info)(void);
void (*jgrf_video_set_info)(jg_videoinfo_t*);
void (*jgrf_video_swapbuffers)(void);
void (*jgrf_video_text)(int, int, const char*);
void (*jgrf_video_rehash)(void);
void *(*jgrf_video_get_pixels)(int*, int*);

static jgrf_gdata_t *gdata;

// Set function pointers for video - Use to select a video API when more exist
void jgrf_video_setfuncs(void) {
    gdata = jgrf_gdata_ptr();
    jg_setting_t *settings = jgrf_settings_ptr();

    switch (settings[VIDEO_API].val) {
        case 0: case 1: // OpenGL - Core Profile and OpenGL ES
            jgrf_video_create = &jgrf_video_gl_create;
            jgrf_video_init = &jgrf_video_gl_init;
            jgrf_video_deinit = &jgrf_video_gl_deinit;
            jgrf_video_fullscreen = &jgrf_video_gl_fullscreen;
            jgrf_video_render = &jgrf_video_gl_render;
            jgrf_video_resize = &jgrf_video_gl_resize;
            jgrf_video_get_scale_params = &jgrf_video_gl_get_scale_params;
            jgrf_video_set_cursor = &jgrf_video_gl_set_cursor;
            jgrf_video_get_info = &jgrf_video_gl_get_info;
            jgrf_video_set_info = &jgrf_video_gl_set_info;
            jgrf_video_swapbuffers = &jgrf_video_gl_swapbuffers;
            jgrf_video_text = &jgrf_video_gl_text;
            jgrf_video_rehash = &jgrf_video_gl_rehash;
            jgrf_video_get_pixels = &jgrf_video_gl_get_pixels;
            break;
        case 2: // OpenGL - Compatibility Profile
            jgrf_video_create = &jgrf_video_gl_create;
            jgrf_video_init = &jgrf_video_gl_init;
            jgrf_video_deinit = &jgrf_video_gl_deinit;
            jgrf_video_fullscreen = &jgrf_video_gl_fullscreen;
            jgrf_video_render = &jgrf_video_gl_render_compat;
            jgrf_video_resize = &jgrf_video_gl_resize;
            jgrf_video_get_scale_params = &jgrf_video_gl_get_scale_params;
            jgrf_video_set_cursor = &jgrf_video_gl_set_cursor;
            jgrf_video_get_info = &jgrf_video_gl_get_info;
            jgrf_video_set_info = &jgrf_video_gl_set_info;
            jgrf_video_swapbuffers = &jgrf_video_gl_swapbuffers;
            jgrf_video_text = &jgrf_video_gl_text;
            jgrf_video_rehash = &jgrf_video_gl_rehash;
            jgrf_video_get_pixels = &jgrf_video_gl_get_pixels;
            break;
#ifdef JGRF_VULKAN
        case 3: // Vulkan
            jgrf_video_create = &jgrf_video_vk_create;
            jgrf_video_init = &jgrf_video_vk_init;
            jgrf_video_deinit = &jgrf_video_vk_deinit;
            jgrf_video_fullscreen = &jgrf_video_vk_fullscreen;
            jgrf_video_render = &jgrf_video_vk_render;
            jgrf_video_resize = &jgrf_video_vk_resize;
            jgrf_video_get_scale_params = &jgrf_video_vk_get_scale_params;
            jgrf_video_set_cursor = &jgrf_video_vk_set_cursor;
            jgrf_video_get_info = &jgrf_video_vk_get_info;
            jgrf_video_set_info = &jgrf_video_vk_set_info;
            jgrf_video_swapbuffers = &jgrf_video_vk_swapbuffers;
            jgrf_video_text = &jgrf_video_vk_text;
            jgrf_video_rehash = &jgrf_video_vk_rehash;
            jgrf_video_get_pixels = &jgrf_video_vk_get_pixels;
            break;
#endif
        default:
            jgrf_log(JG_LOG_ERR, "Invalid Video API: %d\n",
                settings[VIDEO_API].val);
            break;
    }
}

// Load an application icon
void jgrf_video_icon_load(SDL_Window *window) {
    char iconpath[192];
#ifdef __APPLE__
    int iconsize = 1024;
#else
    int iconsize = 96;
#endif

#ifdef JGRF_STATIC
    snprintf(iconpath, sizeof(iconpath), "%s%cicons%c%s%d.png",
        jgrf_gdata_ptr()->binpath, SEP, SEP, jg_get_coreinfo("")->name,
        iconsize);

#if defined(DATADIR)
    struct stat fbuf; // Make sure the icon actually exists at this path
    if (stat(iconpath, &fbuf) != 0) { // Not found locally, use system-wide path
        snprintf(iconpath, sizeof(iconpath),
            "%s%cjollygood%c%s%c%s%d.png",
            DATADIR, SEP, SEP, jg_get_coreinfo("")->name, SEP,
            jg_get_coreinfo("")->name, iconsize);
    }
#endif

#else
    snprintf(iconpath, sizeof(iconpath), "%s%cicons%cjollygood%d.png",
        jgrf_gdata_ptr()->binpath, SEP, SEP, iconsize);

#if defined(DATADIR)
    struct stat fbuf; // Make sure the icon actually exists at this path
    if (stat(iconpath, &fbuf) != 0) { // Not found locally, use system-wide path
        snprintf(iconpath, sizeof(iconpath),
            "%s%cjollygood%cjgrf%cjollygood%d.png",
            DATADIR, SEP, SEP, SEP, iconsize);
    }
#endif

#endif

    uint32_t x, y;
    uint8_t *png_icon = 0;
    uint8_t error = lodepng_decode32_file(&png_icon, &x, &y, iconpath);
    if (error)
        jgrf_log(JG_LOG_WRN, "lodepng code %u: %s\n",
            error, lodepng_error_text(error));

    SDL_Surface *icon;
    icon = SDL_CreateSurfaceFrom(x, y, SDL_PIXELFORMAT_RGBA32, png_icon,
        x * sizeof(uint32_t));

    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
    free(png_icon);

    // Seed the RNG
    srand((unsigned)time(NULL));
}

// Write the currently displayed video frame to a .png file
void jgrf_video_screenshot(void) {
    char ssname[256];
    snprintf(ssname, sizeof(ssname), "%s%d-%03x.png",
        gdata->sspath, (unsigned)time(NULL), rand() % 0xfff);

    // Rendered pixels after post-processing
    int rw, rh;
    void *ssdata = jgrf_video_get_pixels(&rw, &rh);

    // Set up the LodePNGState to ensure 100% opacity on the alpha channel
    LodePNGState state;
    lodepng_state_init(&state);
    state.info_raw.colortype = LCT_RGBA;
    state.info_raw.bitdepth = 8;
    state.info_png.color.colortype = LCT_RGB;
    state.info_png.color.bitdepth = 8;
    state.encoder.auto_convert = 0;

    // Dump the raw data into PNG format
    unsigned char *png = NULL;
    size_t pngsize = 0;
    unsigned error = lodepng_encode(&png, &pngsize, ssdata, rw, rh, &state);

    // Save the screenshot to the filesystem if encoding was successful
    if (!error)
        error = lodepng_save_file(png, pngsize, ssname);

    if (error)
        jgrf_log(JG_LOG_WRN, "lodepng code %u: %s\n",
            error, lodepng_error_text(error));
    else
        jgrf_log(JG_LOG_SCR, "Screenshot saved");

    lodepng_state_cleanup(&state);
    free(png);
    free(ssdata);
}
