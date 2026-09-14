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

#ifndef OSD_H
#define OSD_H

#include <stdint.h>

/* Fixed canvas dimensions. The OSD always rasterises into a 640x480 RGBA8888
   buffer in its own logical pixel space. The renderer composites a portion of
   this canvas over the post-shader output as an alpha-blended quad, with
   integer scaling determined by the current render dimensions.
*/
#define JGRF_OSD_W 640
#define JGRF_OSD_H 480

// Text slots
enum {
    JGRF_OSD_TEXT_FRONTEND = 0, // Frontend messages (bottom left)
    JGRF_OSD_TEXT_CORE     = 1, // Core messages (bottom right)
    JGRF_OSD_TEXT_MENU     = 2, // Menu / Input Config (top left)
    JGRF_OSD_TEXT_AUX      = 3, // Reserved for future use
    JGRF_OSD_NUM_TEXT_SLOTS
};

// Pixel effects
enum {
    JGRF_OSD_FX_TEA  = 0,
    JGRF_OSD_FX_SNOW = 1,
    JGRF_OSD_FX_MUSH = 2,
    JGRF_OSD_FX_FIRE = 3,
    JGRF_OSD_FX_DMK = 4,
    JGRF_OSD_NUM_FX
};

// Text positioning anchor
enum {
    JGRF_OSD_ANCHOR_TL = 0,
    JGRF_OSD_ANCHOR_TR = 1,
    JGRF_OSD_ANCHOR_BL = 2,
    JGRF_OSD_ANCHOR_BR = 3
};

void jgrf_osd_init(void);
void jgrf_osd_deinit(void);
void jgrf_osd_rehash(void);
void jgrf_osd_toggle(int active);

void jgrf_osd_set_visible(int vis_w, int vis_h);

// Shade layer - set an RGBA value to shade, or 0 disable
void jgrf_osd_shade_set(uint32_t rgba);

// Effects layer
void jgrf_osd_fx_enable(int fx_id);
void jgrf_osd_fx_disable(int fx_id);
void jgrf_osd_fx_clear(void);

/* Text layer: (X, Y) is in OSD canvas pixels, interpreted relative to the
   anchor corner of the visible region. frames=0 means inactive, frames>0
   means countdown, frames=-1 means persistent until cleared. Pass NULL or
   empty string with frames=0 to clear.
*/
void jgrf_osd_text(int slot, int frames, int anchor, int x, int y,
    uint32_t rgba, const char *text);

void jgrf_osd_text_clear(int slot);

// Per-frame: advance effects, decrement text countdowns, perform redraws
void jgrf_osd_step(void);

/* Composite all active layers into the canvas and return a pointer to the
   RGBA8888 pixel data. *active is set to 1 if anything was drawn (renderer
   should draw the OSD quad), 0 otherwise (renderer should skip).
   *dirty is set to 1 if the canvas content changed since the last call
   (renderer should re-upload the texture); 0 otherwise.
*/
const void *jgrf_osd_composite(int *active, int *dirty);

#endif
