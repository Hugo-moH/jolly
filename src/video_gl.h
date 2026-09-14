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

#ifndef VIDEO_GL_H
#define VIDEO_GL_H

void jgrf_video_gl_create(void);
int jgrf_video_gl_init(void);
void jgrf_video_gl_deinit(void);
void jgrf_video_gl_fullscreen(void);
void jgrf_video_gl_render(int);
void jgrf_video_gl_render_compat(int);
void jgrf_video_gl_resize(void);
void jgrf_video_gl_get_scale_params(float*, float*, float*, float*);
void jgrf_video_gl_set_cursor(int);
jg_videoinfo_t* jgrf_video_gl_get_info(void);
void jgrf_video_gl_set_info(jg_videoinfo_t*);
void jgrf_video_gl_setup(void);
void jgrf_video_gl_setup_compat(void);
void jgrf_video_gl_swapbuffers(void);
void jgrf_video_gl_text(int, int, const char*);
void *jgrf_video_gl_get_pixels(int*, int*);

void jgrf_video_gl_rehash(void);
#endif
