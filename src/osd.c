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

/*
 * The Jolly Good On-Screen Display
 *
 * The OSD owns a fixed-size 640x480 RGBA8888 canvas. Three logical layers are
 * composited into it on demand:
 *
 *   1. Shade   - uniform colour + alpha covering the visible region
 *   2. Effects - animated software pixel effects (steam, snow, ...)
 *   3. Text    - bitmap font glyphs in fixed slots
 *
 * The composite order is shade -> effects -> text, matching the visual
 * intent: shade darkens the game underneath, effects appear over the dimmed
 * game, and text remains crisp on top. Each layer can be on or off in any
 * combination.
 *
 * The canvas is API-agnostic: the renderer treats it as a single RGBA8888
 * texture and draws it as one alpha-blended quad over the post-shader output,
 * at an integer scale determined by the current render dimensions. The OSD
 * itself never sees the game pixels - the alpha blending happens on the GPU
 * during the final draw, not in software.
 *
 * The visible region (vis_w, vis_h) is the portion of the canvas that the
 * renderer actually shows. Drawing operations clip to this region so that
 * shade, effects and text never extend outside the visible game area, even
 * when the game render area is smaller than the canvas.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "jg/jg.h"

#include "jgrf.h"
#include "osd.h"
#include "settings.h"

#include "font/6x8.h"
#include "font/8x8.h"

#define JGRF_OSD_ORANGE 0xd45500ff
#define JGRF_OSD_TEXT_MAX 512
#define JGRF_OSD_LINE_GAP 2

static int osdactive = 0;
static uint32_t osd_text_rgba = JGRF_OSD_ORANGE;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

/* The OSD canvas - RGBA8888, 0xAABBGGRR in little-endian byte order when read
   as uint32_t. It is stored as packed bytes so it can be uploaded directly as
   GL_RGBA / GL_UNSIGNED_BYTE without endianness concerns.
*/
static uint8_t canvas[JGRF_OSD_W * JGRF_OSD_H * 4];

// Current visible region, defaults to the full canvas
static int vis_w = JGRF_OSD_W;
static int vis_h = JGRF_OSD_H;

static jg_setting_t *settings = NULL;

// Bitmap font state
static uint8_t *font = NULL;
static unsigned fwidth = 8;
static unsigned fheight = 8;

/* ------------------------------------------------------------------------ */
/* Shade layer                                                              */
/* ------------------------------------------------------------------------ */

static uint32_t shade_rgba = 0; // 0 == off

void jgrf_osd_shade_set(uint32_t rgba) {
    shade_rgba = rgba;
}

/* ------------------------------------------------------------------------ */
/* Effects layer                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    void (*init)(void);
    void (*step)(int vw, int vh);
    void (*deinit)(void);
} jgrf_osd_fx_t;

static void fx_tea_init(void);
static void fx_tea_step(int vw, int vh);
static void fx_tea_deinit(void);

static void fx_snow_init(void);
static void fx_snow_step(int vw, int vh);
static void fx_snow_deinit(void);

static void fx_mush_init(void);
static void fx_mush_step(int vw, int vh);
static void fx_mush_deinit(void);

static void fx_fire_init(void);
static void fx_fire_step(int vw, int vh);
static void fx_fire_deinit(void);

static void fx_dmk_init(void);
static void fx_dmk_step(int vw, int vh);
static void fx_dmk_deinit(void);

static const jgrf_osd_fx_t fx_table[JGRF_OSD_NUM_FX] = {
    [JGRF_OSD_FX_TEA] = { fx_tea_init, fx_tea_step, fx_tea_deinit },
    [JGRF_OSD_FX_SNOW] = { fx_snow_init, fx_snow_step, fx_snow_deinit },
    [JGRF_OSD_FX_MUSH] = { fx_mush_init, fx_mush_step, fx_mush_deinit },
    [JGRF_OSD_FX_FIRE] = { fx_fire_init, fx_fire_step, fx_fire_deinit },
    [JGRF_OSD_FX_DMK] = { fx_dmk_init, fx_dmk_step, fx_dmk_deinit }
};

static unsigned fx_active[JGRF_OSD_NUM_FX];

void jgrf_osd_fx_enable(int fx_id) {
    if (fx_id < 0 || fx_id >= JGRF_OSD_NUM_FX || fx_active[fx_id])
        return;

    fx_active[fx_id] = 1;

    if (fx_table[fx_id].init)
        fx_table[fx_id].init();
}

void jgrf_osd_fx_disable(int fx_id) {
    if (fx_id < 0 || fx_id >= JGRF_OSD_NUM_FX || !fx_active[fx_id])
        return;

    fx_active[fx_id] = 0;

    if (fx_table[fx_id].deinit)
        fx_table[fx_id].deinit();
}

void jgrf_osd_fx_clear(void) {
    for (size_t i = 0; i < JGRF_OSD_NUM_FX; ++i)
        jgrf_osd_fx_disable(i);
}

static int fx_any_active(void) {
    for (size_t i = 0; i < JGRF_OSD_NUM_FX; ++i)
        if (fx_active[i]) return 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Text layer                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    int frames; // 0 = Inactive, >0 = countdown, -1 = persistent
    int anchor;
    int x, y;
    uint32_t rgba;
    char text[JGRF_OSD_TEXT_MAX];
} jgrf_osd_text_slot_t;

static jgrf_osd_text_slot_t text_slots[JGRF_OSD_NUM_TEXT_SLOTS];

void jgrf_osd_text(int slot, int frames, int anchor, int x, int y,
    uint32_t rgba, const char *text) {

    if (slot < 0 || slot >= JGRF_OSD_NUM_TEXT_SLOTS)
        return;

    jgrf_osd_text_slot_t *s = &text_slots[slot];
    s->frames = frames;
    s->anchor = anchor;
    s->x = x;
    s->y = y;
    s->rgba = rgba ? rgba : osd_text_rgba;

    if (text)
        snprintf(s->text, sizeof(s->text), "%s", text);
    else
        s->text[0] = '\0';
}

void jgrf_osd_text_clear(int slot) {
    if (slot < 0 || slot >= JGRF_OSD_NUM_TEXT_SLOTS)
        return;
    text_slots[slot].frames = 0;
    text_slots[slot].text[0] = '\0';
}

/* Compute the display width of a string in canvas pixels. Newlines start a
   new line; the returned width is the longest single line. */
static int text_pixel_width(const char *text) {
    int max_w = 0;
    int cur_w = 0;

    for (size_t c = 0; text[c] != '\0'; ++c) {
        if (text[c] == '\n') {
            if (cur_w > max_w)
                max_w = cur_w;
            cur_w = 0;
            continue;
        }
        cur_w += fwidth;
    }

    if (cur_w > max_w)
        max_w = cur_w;

    return max_w;
}

// Compute the display height of a string in canvas pixels.
static int text_pixel_height(const char *text) {
    int lines = 1;
    for (size_t c = 0; text[c] != '\0'; ++c)
        if (text[c] == '\n') ++lines;
    return lines * ((int)fheight + JGRF_OSD_LINE_GAP);
}

// Resolve a slot's anchor
static void resolve_anchor(jgrf_osd_text_slot_t *s, int *out_x, int *out_y) {
    int tw = text_pixel_width(s->text);
    int th = text_pixel_height(s->text);
    int ax = 0;
    int ay = 0;

    switch (s->anchor) {
        default:
        case JGRF_OSD_ANCHOR_TL: {
            ax = s->x;
            ay = s->y;
            break;
        }
        case JGRF_OSD_ANCHOR_TR: {
            ax = vis_w - s->x - tw;
            ay = s->y;
            break;
        }
        case JGRF_OSD_ANCHOR_BL: {
            ax = s->x;
            ay = vis_h - s->y - th;
            break;
        }
        case JGRF_OSD_ANCHOR_BR: {
            ax = vis_w - s->x - tw;
            ay = vis_h - s->y - th;
            break;
        }
    }

    *out_x = ax;
    *out_y = ay;
}

static int text_active(void) {
    for (size_t i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i) {
        const jgrf_osd_text_slot_t *s = &text_slots[i];
        if (s->frames != 0 && s->text[0] != '\0')
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Canvas primitives                                                        */
/* ------------------------------------------------------------------------ */

// Write an opaque pixel, no clipping check
static inline void canvas_set(int x, int y, uint32_t rgba) {
    uint8_t *p = &canvas[(y * JGRF_OSD_W + x) * 4];
    p[0] = (rgba >> 24) & 0xff; // R
    p[1] = (rgba >> 16) & 0xff; // G
    p[2] = (rgba >> 8)  & 0xff; // B
    p[3] =  rgba        & 0xff; // A
}

// Blend: dst = src * src.a + dst * (1 - src.a) -- src is given as 0xRRGGBBAA
static inline void canvas_blend(int x, int y, uint32_t rgba) {
    uint8_t sa = rgba & 0xff;
    if (sa == 0)
        return;

    if (sa == 0xff) {
        canvas_set(x, y, rgba);
        return;
    }

    uint8_t *p = &canvas[(y * JGRF_OSD_W + x) * 4];
    uint16_t sr = (rgba >> 24) & 0xff;
    uint16_t sg = (rgba >> 16) & 0xff;
    uint16_t sb = (rgba >> 8)  & 0xff;
    uint16_t da = p[3];

    /* Standard "over" with straight alpha. The canvas alpha grows toward
       opaque as more layers are added, which matches the renderer's
       SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend at composite time.
    */
    uint16_t inv = 255 - sa;
    p[0] = (uint8_t)((sr * sa + p[0] * inv + 127) / 255);
    p[1] = (uint8_t)((sg * sa + p[1] * inv + 127) / 255);
    p[2] = (uint8_t)((sb * sa + p[2] * inv + 127) / 255);
    p[3] = (uint8_t)(sa + (da * inv + 127) / 255);
}

// Clear the canvas to fully transparent
static void canvas_clear(void) {
    memset(canvas, 0, sizeof(canvas));
}

// Fill the visible region with a colour (alpha included)
static void canvas_fill_visible(uint32_t rgba) {
    uint8_t r = (rgba >> 24) & 0xff;
    uint8_t g = (rgba >> 16) & 0xff;
    uint8_t b = (rgba >> 8)  & 0xff;
    uint8_t a =  rgba        & 0xff;

    for (int y = 0; y < vis_h; ++y) {
        uint8_t *row = &canvas[(y * JGRF_OSD_W) * 4];
        for (int x = 0; x < vis_w; ++x) {
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = a;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Bitmap font rasteriser                                                   */
/* ------------------------------------------------------------------------ */

/* Draws text into the canvas at (xo, yo) with the active bitmap font. Glyphs
   that would extend outside the visible region are clipped. A single-pixel
   black outline is drawn behind each glyph for legibility against arbitrary
   backgrounds. */
static void jgrf_osd_render_text(int xo, int yo, uint32_t rgba,
    const char *text) {

    int x = xo;
    int y = yo;

    for (size_t c = 0; text[c] != '\0'; ++c) {
        if (text[c] == '\n') {
            y += fheight + JGRF_OSD_LINE_GAP;
            x = xo;
            continue;
        }

        // Skip glyphs entirely outside the visible region
        if (x + (int)fwidth <= 0 || x >= vis_w ||
            y + (int)fheight <= 0 || y >= vis_h) {
            x += fwidth;
            continue;
        }

        const uint8_t *bitmap = &font[(uint8_t)text[c] << 3];

        /* First pass: draw outline. Blacken the 8 neighbours of every set
           pixel, but only where the neighbour itself is unset, so the outline
           borders the glyph without thickening the strokes.
        */
        for (unsigned gy = 0; gy < fheight; ++gy) {
            uint8_t row = bitmap[gy];
            for (unsigned gx = 0; gx < fwidth; ++gx) {
                if (!(row & (1 << (7 - gx))))
                    continue;

                // Set pixel - lay down the outline around it
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0)
                            continue;

                        int px = x + (int)gx + dx;
                        int py = y + (int)gy + dy;

                        if (px < 0 || px >= vis_w ||
                            py < 0 || py >= vis_h) continue;

                        /* Only stamp if the canvas there isn't already part
                           of the glyph itself. Test by checking whether the
                           source bitmap has a pixel at this offset - if it
                           does, the inner pass write will cover it.
                        */
                        int srcx = (int)gx + dx;
                        int srcy = (int)gy + dy;

                        if (srcx >= 0 && srcx < (int)fwidth &&
                            srcy >= 0 && srcy < (int)fheight &&
                            (bitmap[srcy] & (1 << (7 - srcx))))
                            continue;
                        canvas_set(px, py, 0x000000ff);
                    }
                }
            }
        }

        // Second pass: draw the glyph itself in the foreground colour
        for (unsigned gy = 0; gy < fheight; ++gy) {
            uint8_t row = bitmap[gy];
            for (unsigned gx = 0; gx < fwidth; ++gx) {
                if (!(row & (1 << (7 - gx))))
                    continue;

                int px = x + (int)gx;
                int py = y + (int)gy;

                if (px < 0 || px >= vis_w || py < 0 || py >= vis_h)
                    continue;

                canvas_set(px, py, rgba);
            }
        }

        x += fwidth;
    }
}

/* ------------------------------------------------------------------------ */
/* Effect implementations                                                   */
/* ------------------------------------------------------------------------ */

/* Tea - Canonical demoscene plasma. At each pixel, sum four sinusoids:
   one in x, one in y, one rotated diagonal, and one radial from the
   visible-region centre. Each is animated by a per-frame phase. The sum
   is mapped into a 256-entry palette of warm tea-amber tones so it reads
   as swirling tea rather than the usual rainbow.

   The radial term is computed inline from the visible-region centre so
   the rings are always centred on screen regardless of the emulated
   system's output size. A fixed-point scale factor adapts ring density
   to the current resolution. The diagonal uses x + 2y (atan 1/2,
   roughly 27 degrees) instead of x + y (45 degrees) to break grid-axis
   symmetry. A slowly cycling spatial drift offsets the x and y terms so
   the pattern wanders across the screen.

   Composited at moderate alpha so the underlying shade still reads.
*/

// Palette containing 256 entries of 0xRRGGBB (alpha applied at blend)
static uint32_t plasma_palette[256];

// Sine LUT, signed 8-bit. Range: [-127, +127]
static int8_t plasma_sin[256];

/* Phase accumulators, advanced once per frame. */
static uint8_t plasma_pa = 0; // sin(x) animation
static uint8_t plasma_pb = 0; // sin(y) animation
static uint8_t plasma_pc = 0; // sin(diagonal) animation
static uint8_t plasma_pd = 0; // sin(radial) animation
static uint8_t plasma_pe = 0; // spatial drift

static void fx_tea_init(void) {
    for (int i = 0; i < 256; ++i) {
        double theta = (double)i * (2.0 * 3.14159265358979323846 / 256.0);
        plasma_sin[i] = (int8_t)(sin(theta) * 127.0);
    }

    const uint8_t stops[5][3] = {
        { 0x24, 0x0c, 0x05 }, // dark warm brown - troughs
        { 0x4a, 0x1a, 0x07 }, // deep umber
        { 0x80, 0x36, 0x0d }, // dark brown
        { 0xb4, 0x4e, 0x12 }, // brewed mahogany
        { 0xd4, 0x76, 0x20 }  // saturated amber - highlight
    };

    const int n_stops = 5;
    const int seg = 256 / (n_stops - 1); // 64

    for (int i = 0; i < 256; ++i) {
        int s = i / seg;
        if (s >= n_stops - 1)
            s = n_stops - 2;
        int t = i - s * seg; // 0..63
        const uint8_t *a = stops[s];
        const uint8_t *b = stops[s + 1];
        int r = a[0] + ((b[0] - a[0]) * t) / seg;
        int g = a[1] + ((b[1] - a[1]) * t) / seg;
        int bl = a[2] + ((b[2] - a[2]) * t) / seg;
        plasma_palette[i] = ((uint32_t)r << 24) |
                            ((uint32_t)g << 16) |
                            ((uint32_t)bl << 8);
    }
}

static void fx_tea_deinit(void) {
    // Nothing allocated
}

static void fx_tea_step(int vw, int vh) {
    if (vw < 1 || vh < 1)
        return;
    if (vw > JGRF_OSD_W) vw = JGRF_OSD_W;
    if (vh > JGRF_OSD_H) vh = JGRF_OSD_H;

    plasma_pa += 2;
    plasma_pb += 1;
    plasma_pc += 3;
    plasma_pd += 1;
    plasma_pe += 1;

    const uint8_t alpha = 0x50;

    /* Radial term: centred on the visible region with adaptive ring
       density. The fixed-point multiplier scales squared distance so
       that ~5 full sine cycles fit across the half-diagonal, keeping
       ring count roughly constant at any resolution.
    */
    int cx = vw >> 1;
    int cy = vh >> 1;
    int half_diag_sq = cx * cx + cy * cy;
    int rad_fp = (half_diag_sq > 0)
        ? ((5 * 256) << 16) / half_diag_sq // 16.16 fixed-point
        : (1 << 16);

    /* Spatial drift: the x and y terms are offset by a slowly cycling
       sine pair 90 degrees apart, tracing a gentle circular wander so
       the whole pattern drifts rather than breathing in place.
    */
    int drift_x = plasma_sin[plasma_pe] >> 2;                    // +/-31 px
    int drift_y = plasma_sin[(uint8_t)(plasma_pe + 64)] >> 2;    // 90 deg off

    for (int y = 0; y < vh; ++y) {
        int dy = y - cy;
        int dy2 = dy * dy;

        // Vertical term, hoisted -- depends only on y
        int sy = plasma_sin[(uint8_t)((y << 1) + plasma_pb + drift_y)];

        for (int x = 0; x < vw; ++x) {
            int dx = x - cx;

            // Horizontal, with spatial drift
            int sx = plasma_sin[(uint8_t)((x << 1) + plasma_pa + drift_x)];

            // Rotated diagonal (x + 2y) -- ~27 deg, breaks 45 deg symmetry
            int sxy = plasma_sin[(uint8_t)(x + (y << 1) + plasma_pc)];

            // Radial from visible centre, resolution-adaptive density
            uint8_t rad = (uint8_t)((((dx * dx + dy2) * rad_fp) >> 16));
            int sr = plasma_sin[(uint8_t)(rad + plasma_pd)];

            /* Sum of four [-127, +127] terms -> [-508, +508]. Shift to
               [0, 1016] and divide to land in [0, 255].
            */
            int idx = (sx + sy + sxy + sr + 508) >> 2;
            if (idx > 255) idx = 255;

            canvas_blend(x, y, plasma_palette[idx] | alpha);
        }
    }
}

/* Snow - Shamelessly inspired by ZSNES. A flock of small white particles
   falling at varied speeds with slight horizontal drift. Flakes are anywhere
   between half opaque and fully opaque.
*/
#define FX_SNOW_FLAKES 256

typedef struct {
    float fx;       // Fractional X
    float fy;       // Fractional Y
    float speed;    // Sub-pixel Y advance per frame
    float drift;    // Sub-pixel X advance per frame
    uint8_t alpha;  // 0x80..0xff
} snow_flake_t;

static snow_flake_t snow_flakes[FX_SNOW_FLAKES];
static uint32_t snow_rng = 0x12345678;

static void fx_snow_init(void) {
    for (size_t i = 0; i < FX_SNOW_FLAKES; ++i) {
        snow_flakes[i].fx =
            xorshift32(&snow_rng) % (vis_w > 0 ? vis_w : 1);
        snow_flakes[i].fy =
            xorshift32(&snow_rng) % (vis_h > 0 ? vis_h : 1);

        // Speeds in [0.5, 1.75]
        snow_flakes[i].speed =
            0.5f + ((xorshift32(&snow_rng) % 100) / 100.0f) * 1.25f;

        // Drift in [-0.4, +0.4]
        snow_flakes[i].drift =
            (((xorshift32(&snow_rng) % 100) / 100.0f) - 0.5f) * 0.8f;

        snow_flakes[i].alpha = 0x80 + (xorshift32(&snow_rng) % 0x80);
    }
}

static void fx_snow_deinit(void) {
    // Nothing allocated
}

static void fx_snow_step(int vw, int vh) {
    for (size_t i = 0; i < FX_SNOW_FLAKES; ++i) {
        snow_flake_t *f = &snow_flakes[i];

        /* Reseed any flake whose start-of-frame position is outside the
           visible region into a uniform spot inside it. The init function
           scatters flakes across the full canvas because it runs before
           the visible region is set by the renderer; without this reseed
           those out-of-region flakes get clamped to top/edges by the
           per-frame wrap logic below and produce visible clusters around
           the rim of the visible area for the first second or so.
        */
        if (f->fx < 0.0f || f->fx >= (float)vw ||
            f->fy < 0.0f || f->fy >= (float)vh) {
            f->fx = (float)(xorshift32(&snow_rng) % (vw > 0 ? vw : 1));
            f->fy = (float)(xorshift32(&snow_rng) % (vh > 0 ? vh : 1));
        }

        f->fy += f->speed;
        f->fx += f->drift;
        int x = f->fx;
        int y = f->fy;

        // Wrap around
        if (y >= vh) {
            f->fy = 0.0f;
            y = 0;
            f->fx = (float)(xorshift32(&snow_rng) % (vw > 0 ? vw : 1));
            x = (int)f->fx;
        }

        if (x < 0) {
            f->fx = (float)(vw - 1);
            x = vw - 1;
        }

        if (x >= vw) {
            f->fx = 0.0f;
            x = 0;
        }

        if (x >= 0 && x < vw && y >= 0 && y < vh) {
            uint32_t col = 0xffffff00 | f->alpha;
            canvas_blend(x, y, col);
        }
    }
}

/* Mushrooms - Amanita mushrooms scattered across the visible canvas, each on
   its own lifecycle: grow vertically from nothing, hold at full size, fade
   out, respawn elsewhere. Each mushroom is a filled half-ellipse cap speckled
   with cream spots, atop a filled trapezoidal stem. Lifecycles are staggered
   so they don't all pop together.
*/
#define FX_MUSH_COUNT      32  // Number of mushrooms on screen
#define FX_MUSH_GROW       120 // Frames spent growing
#define FX_MUSH_HOLD       180 // Frames at full size
#define FX_MUSH_FADE       90  // Frames spent fading out
#define FX_MUSH_LIFE       (FX_MUSH_GROW + FX_MUSH_HOLD + FX_MUSH_FADE)
#define FX_MUSH_SPOT_MIN   8   // Minimum Amanita spots per cap
#define FX_MUSH_SPOT_MAX   18  // Maximum Amanita spots per cap

// Colours as 0xRRGGBB with alpha applied per frame from the lifecycle envelope
//#define FX_MUSH_CAP_RGB    0xd45500 // JG Orange, A. muscaria var. guessowii
#define FX_MUSH_CAP_RGB    0xd62828 // Red, A. muscaria
#define FX_MUSH_STEM_RGB   0xf0e8dd
#define FX_MUSH_SPOT_RGB   FX_MUSH_STEM_RGB

/* Spot position in cap-local normalized coords -- nx is in [-1, 1] across
   the cap horizontal radius, ny is in [-1, 0] from cap apex (-1) to
   cap base (0). Stored at spawn so spots are stable between frames.
*/
typedef struct {
    float nx;
    float ny;
} mush_spot_t;

typedef struct {
    int base_x;     // Horizontal center in canvas pixels
    int base_y;     // Y of the bottom of the stem
    int cap_rx;     // Cap horizontal radius at full size
    int cap_ry;     // Cap vertical radius at full size
    int stem_h;     // Stem height at full size
    int stem_top_w; // Stem top half-width
    int stem_bot_w; // Stem bottom half-width
    int age;        // Frames since spawn
    int nspots;     // Number of Amanita spots on this cap
    mush_spot_t spots[FX_MUSH_SPOT_MAX];
} mushroom_t;

static mushroom_t mushrooms[FX_MUSH_COUNT];
static uint32_t mush_rng = 0xfeedface;

static float mush_randf(void) {
    return (xorshift32(&mush_rng) & 0xffff) / 65535.0f;
}

static void mush_spawn(mushroom_t *m, int vw, int vh, int initial_age) {
    /* Pick a cap width first, then derive everything else. Cap widths in
       a range that makes mushrooms feel modest at small canvases and
       still varied at larger ones.
    */
    int cap_rx_min = 12;
    int cap_rx_max = 20;

    if (vw < 160) {
        cap_rx_min = 6;
        cap_rx_max = 12;
    }

    int cap_rx = cap_rx_min +
        (int)(mush_randf() * (float)(cap_rx_max - cap_rx_min));

    // Cap height ~0.4..0.6 of cap width
    int cap_ry = (int)((float)cap_rx * (0.4f + mush_randf() * 0.2f));
    if (cap_ry < 4)
        cap_ry = 4;

    // Stem height ~0.8..1.4 of cap width
    int stem_h = (int)((float)cap_rx * (0.8f + mush_randf() * 0.6f));
    if (stem_h < 6)
        stem_h = 6;

    /* Stem top half-width ~0.22..0.38 of cap width, bottom slightly wider for
       a gentle taper.
    */
    int stem_top_w = (int)((float)cap_rx * (0.22f + mush_randf() * 0.16f));
    if (stem_top_w < 2)
        stem_top_w = 2;
    int stem_bot_w = stem_top_w + 1 + (int)(mush_randf() * 2.0f);

    m->cap_rx = cap_rx;
    m->cap_ry = cap_ry;
    m->stem_h = stem_h;
    m->stem_top_w = stem_top_w;
    m->stem_bot_w = stem_bot_w;
    m->age = initial_age;

    /* Generate single pixel Amanita spots in cap-local normalized coords.
       Sample the upper unit half-disk with a small inset so spots don't
       land on the rim. Always reach FX_MUSH_SPOT_MIN, try for up to a
       random target between MIN and MAX, but cap effort on the extras.
    */
    int target = FX_MUSH_SPOT_MIN +
        (int)(mush_randf() * (float)(FX_MUSH_SPOT_MAX - FX_MUSH_SPOT_MIN + 1));
    if (target > FX_MUSH_SPOT_MAX)
        target = FX_MUSH_SPOT_MAX;

    m->nspots = 0;
    int attempts = 0;

    while (m->nspots < target) {
        ++attempts;
        if (attempts > 64 && m->nspots >= FX_MUSH_SPOT_MIN)
            break;

        float nx = mush_randf() * 2.0f - 1.0f; // -1..1
        float ny = -mush_randf(); // -1..0 (upper half)

        if (nx * nx + ny * ny > 0.78f)
            continue;

        m->spots[m->nspots].nx = nx;
        m->spots[m->nspots].ny = ny;
        ++m->nspots;
    }

    /* Place the mushroom such that its cap top and stem base fit in the
       visible area with a small margin.
    */
    int margin = 2;
    int min_x = cap_rx + margin;
    int max_x = vw - cap_rx - margin;

    if (max_x <= min_x)
        max_x = min_x + 1;

    int total_h = cap_ry + stem_h;
    int min_y = total_h + margin;
    int max_y = vh - margin;

    if (max_y <= min_y)
        max_y = min_y + 1;

    m->base_x = min_x + (int)(mush_randf() * (float)(max_x - min_x));
    m->base_y = min_y + (int)(mush_randf() * (float)(max_y - min_y));
}

static void fx_mush_init(void) {
    /* Stagger ages so mushrooms pop in/out of phase. */
    for (int i = 0; i < FX_MUSH_COUNT; ++i)
        mush_spawn(&mushrooms[i], vis_w, vis_h,
                   (FX_MUSH_LIFE * i) / FX_MUSH_COUNT);
}

static void fx_mush_deinit(void) {
    // Nothing allocated
}

// Fill a horizontal scanline segment with a colour, blended
static void fx_hline(int x0, int x1, int y, int vw, int vh, uint32_t col) {
    if (y < 0 || y >= vh) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= vw) x1 = vw - 1;
    for (int x = x0; x <= x1; ++x)
        canvas_blend(x, y, col);
}

static void fx_mush_step(int vw, int vh) {
    for (int i = 0; i < FX_MUSH_COUNT; ++i) {
        mushroom_t *m = &mushrooms[i];

        ++m->age;
        if (m->age >= FX_MUSH_LIFE) {
            mush_spawn(m, vw, vh, 0);
            continue;
        }

        /* Lifecycle envelopes --- Growth scales the geometry vertically
           from 0 to 1 during the grow phase and stays at 1 thereafter.
           env scales alpha from 0 up during grow, holds at 1, then ramps
           down during fade.
        */
        float growth, env;
        if (m->age < FX_MUSH_GROW) {
            growth = (float)m->age / (float)FX_MUSH_GROW;
            env = growth;
        }
        else if (m->age < FX_MUSH_GROW + FX_MUSH_HOLD) {
            growth = 1.0f;
            env = 1.0f;
        }
        else {
            growth = 1.0f;
            env = (float)(FX_MUSH_LIFE - m->age) / (float)FX_MUSH_FADE;
            if (env < 0.0f)
                env = 0.0f;
        }

        uint8_t alpha = (uint8_t)(env * 0.6f * 255.0f);
        if (alpha == 0)
            continue;

        uint32_t cap_col  = (FX_MUSH_CAP_RGB  << 8) | alpha;
        uint32_t stem_col = (FX_MUSH_STEM_RGB << 8) | alpha;
        uint32_t spot_col = (FX_MUSH_SPOT_RGB << 8) | alpha;

        /* Current scaled dimensions -- the horizontal cap radius doesn't
           grow as fast as the vertical so the cap "pops open" near the
           end of growth - simulating a mushroom emerging.
        */
        float h_growth = (growth < 0.7f) ? (growth / 0.7f) : 1.0f;
        int stem_h = (int)((float)m->stem_h * growth);
        int cap_ry = (int)((float)m->cap_ry * growth);
        int cap_rx = (int)((float)m->cap_rx * h_growth);
        if (cap_rx < 1) cap_rx = 1;
        if (cap_ry < 1) cap_ry = 1;

        /* Geometry coords -- the cap center is positioned so the cap's
           bottom touches the top of the stem.
        */
        int cx = m->base_x;
        int stem_bot_y = m->base_y;
        int stem_top_y = stem_bot_y - stem_h;
        int cap_cy = stem_top_y;

        /* Stem -- trapezoid from just below the cap down to stem_bot_y.
           Starting one row below stem_top_y means the cap fully owns the
           overlap region. Stem and cap never blend over each other. The
           bottom row is trimmed by one pixel each side for a softer foot.
        */
        int top_w = m->stem_top_w;
        int bot_w = m->stem_bot_w;
        if (stem_h > 0) {
            for (int y = stem_top_y + 1; y <= stem_bot_y; ++y) {
                /* t goes 0 just under the cap, 1 at the bottom. */
                float t = (stem_h > 0)
                    ? (float)(y - stem_top_y) / (float)stem_h : 0.0f;
                int hw = top_w + (int)((float)(bot_w - top_w) * t + 0.5f);
                if (y == stem_bot_y && hw > 1) --hw;
                fx_hline(cx - hw, cx + hw, y, vw, vh, stem_col);
            }
        }

        /* Cap -- half-ellipse centred at (cx, cap_cy). For each row from
           cap_cy - cap_ry up to cap_cy, compute the half-width from
           the ellipse equation: (x/rx)^2 + (y/ry)^2 = 1
           => half_width = rx * sqrt(1 - (y/ry)^2).
           Skip rows with a half-width below 2 so the cap doesn't end in
           a single pixel nub at the apex.
        */
        for (int dy = -cap_ry; dy <= 0; ++dy) {
            float ny = (float)dy / (float)cap_ry;
            float k = 1.0f - ny * ny;
            int hw = (int)((float)cap_rx * sqrtf(k) + 0.5f);
            if (hw < 2)
                continue;
            int yy = cap_cy + dy;
            fx_hline(cx - hw, cx + hw, yy, vw, vh, cap_col);
        }

        /* Amanita spots -- skip while the cap is still a sliver. Each spot
           is a single pixel in cap-local normalized coords, projected onto
           the current scaled ellipse. The spawn-time inset keeps them
           inside the cap silhouette at all sizes.
        */
        if (cap_rx >= 6 && cap_ry >= 4) {
            for (int s = 0; s < m->nspots; ++s) {
                const mush_spot_t *sp = &m->spots[s];
                int sx = cx + (int)(sp->nx * (float)cap_rx + 0.5f);
                int sy = cap_cy + (int)(sp->ny * (float)cap_ry + 0.5f);
                if (sx < 0 || sx >= vw || sy < 0 || sy >= vh)
                    continue;
                canvas_blend(sx, sy, spot_col);
            }
        }
    }
}

/* Fire - Doom-style propagating fire. Per-pixel intensity is stored in a
   palette-indexed buffer the size of the canvas. The bottom row is held at
   maximum heat, seeding the flames. Each frame, every cell takes its value
   from a cell on the row below with a small random horizontal jitter, minus
   a small random cooling. The result is a chaotic plume that rises, cools
   and dissipates, with no per-particle bookkeeping.
*/
#define FX_FIRE_PAL_MAX 36 // Highest palette index (37 entries, 0..36)
static uint32_t fx_fire_palette[FX_FIRE_PAL_MAX + 1];
static uint8_t fx_fire_buf[JGRF_OSD_W * JGRF_OSD_H];
static uint32_t fx_fire_rng = 0xc0ffee42;

static void fx_fire_init(void) {
    memset(fx_fire_buf, 0, sizeof(fx_fire_buf));

    /* stops[i] = { index, R, G, B }, sorted by index ascending.
       Linearly interpolate RGB between consecutive stops.
    */
    const unsigned stops[6][4] = {
        {  0, 0x07, 0x07, 0x07 }, {  4, 0x28, 0x07, 0x07 },
        { 12, 0xa8, 0x1f, 0x07 }, { 24, 0xe0, 0x7b, 0x0b },
        { 32, 0xf4, 0xbb, 0x4b }, { 36, 0xff, 0xef, 0xc7 }
    };

    for (int s = 0; s < 6 - 1; ++s) {
        int i0 = stops[s][0], i1 = stops[s + 1][0];
        for (int i = i0; i <= i1; ++i) {
            float t = (i1 == i0) ? 0.0f : (float)(i - i0) / (float)(i1 - i0);
            unsigned r = stops[s][1] + ((stops[s + 1][1] - stops[s][1]) * t);
            unsigned g = stops[s][2] + ((stops[s + 1][2] - stops[s][2]) * t);
            unsigned b = stops[s][3] + ((stops[s + 1][3] - stops[s][3]) * t);
            fx_fire_palette[i] = (r << 16) | (g << 8) | b;
        }
    }
}

static void fx_fire_deinit(void) {
    // Nothing allocated
}

static void fx_fire_step(int vw, int vh) {
    if (vw < 1 || vh < 1)
        return;
    if (vw > JGRF_OSD_W) vw = JGRF_OSD_W;
    if (vh > JGRF_OSD_H) vh = JGRF_OSD_H;

    /* Seed the bottom row with maximum heat. Holding this every frame is what
       keeps the fire alive.
    */
    uint8_t *seed_row = &fx_fire_buf[(vh - 1) * JGRF_OSD_W];
    for (int x = 0; x < vw; ++x)
        seed_row[x] = FX_FIRE_PAL_MAX;

    /* Propagate upward: each cell on row y writes its value (minus a small
       cooling) to a horizontally jittered cell on row y-1. Iterating from the
       bottom up means sources are read for this frame before they get
       overwritten as destinations from the row below. The same destination
       can be written by multiple sources -- that overlap is part of what
       creates the chaotic plume.
    */
    for (int y = vh - 1; y > 0; --y) {
        const uint8_t *src_row = &fx_fire_buf[y * JGRF_OSD_W];
        uint8_t *dst_row = &fx_fire_buf[(y - 1) * JGRF_OSD_W];
        for (int x = 0; x < vw; ++x) {
            uint8_t pixel = src_row[x];
            if (pixel == 0) {
                dst_row[x] = 0;
                continue;
            }
            uint32_t r = xorshift32(&fx_fire_rng);
            int rand = r & 3; // 0..3 horizontal jitter
            int cool = ((r >> 2) & 7) < 3 ? 1 : 0;
            int dx = x + 2 - rand;
            if (dx < 0) dx = 0;
            if (dx >= vw) dx = vw - 1;
            dst_row[dx] = pixel - cool;
        }
    }

    for (int y = 0; y < vh; ++y) {
        const uint8_t *row = &fx_fire_buf[y * JGRF_OSD_W];
        for (int x = 0; x < vw; ++x) {
            uint8_t idx = row[x];
            if (idx == 0)
                continue;
            uint32_t rgb = fx_fire_palette[idx];
            int a = 0x40 + (idx * (0xff - 0x40)) / FX_FIRE_PAL_MAX;
            if (a > 0xff) a = 0xff;
            canvas_blend(x, y, (rgb << 8) | (uint32_t)a);
        }
    }
}

/* Danmaku - Phyllotactic danmaku bullet emitter. An emitter fires bullets,
   advancing its golden-angle slot by ~137.5078 degrees between shots. Each
   bullet flies radially outward at constant speed. Because the golden angle
   is the "most irrational" rotation, no rational approximation packs the disc
   evenly -- the bullets in flight trace the interlocking spiral pattern of a
   sunflower seedhead. Bullets vanish when they leave the visible rectangle
   (age = -1 marks a free slot). Each frame, one free slot is reused for a new
   bullet at the center.
*/
#define FX_DMK_BULLETS 1024
#define FX_DMK_GOLDEN_ANGLE 2.39996322972865332f // ~137.5078 deg in radians
#define FX_DMK_TWO_PI 6.28318530717958647692f
#define FX_DMK_SPEED 1.2f        // Pixels per frame, radial
#define FX_DMK_RADIUS 1          // Diamond half-width
#define FX_DMK_CULL_AGE 2        // Skip drawing while age < this
#define FX_DMK_SPAWN_PER_FRAME 8 // Bullets emitted per frame

static float fx_dmk_theta[FX_DMK_BULLETS]; // Wrapped angle, valid if age >= 0
static int   fx_dmk_age[FX_DMK_BULLETS];   // -1 = free slot
static float fx_dmk_next_theta = 0.0f;     // Next slot's angle

static void fx_dmk_init(void) {
    for (int i = 0; i < FX_DMK_BULLETS; ++i)
        fx_dmk_age[i] = -1;
    fx_dmk_next_theta = 0.0f;
}

static void fx_dmk_deinit(void) {
    // Nothing allocated
}

static void fx_dmk_step(int vw, int vh) {
    if (vw < 1 || vh < 1)
        return;

    float cx = vw * 0.666666f;
    float cy = vh * 0.333333f;

    // Spawn up to N bullets per frame into the first free slots
    int spawned = 0;
    for (int i = 0; i < FX_DMK_BULLETS &&
                    spawned < FX_DMK_SPAWN_PER_FRAME; ++i) {
        if (fx_dmk_age[i] < 0) {
            fx_dmk_theta[i] = fx_dmk_next_theta;
            fx_dmk_age[i] = 0;

            // Advance to next slot, keeping theta wrapped in [0, 2pi)
            fx_dmk_next_theta += FX_DMK_GOLDEN_ANGLE;
            if (fx_dmk_next_theta >= FX_DMK_TWO_PI)
                fx_dmk_next_theta -= FX_DMK_TWO_PI;
            ++spawned;
        }
    }

    // Advance and draw all live bullets
    for (int i = 0; i < FX_DMK_BULLETS; ++i) {
        if (fx_dmk_age[i] < 0)
            continue;

        fx_dmk_age[i] += 1;

        float r = FX_DMK_SPEED * (float)fx_dmk_age[i];
        float theta = fx_dmk_theta[i];
        int x = (int)(cx + r * cosf(theta));
        int y = (int)(cy + r * sinf(theta));

        // Bullet has left the visible rectangle: mark slot free
        if (x < 0 || x >= vw || y < 0 || y >= vh) {
            fx_dmk_age[i] = -1;
            continue;
        }

        // Skip drawing while too close to centre to avoid clumping
        if (fx_dmk_age[i] < FX_DMK_CULL_AGE)
            continue;

        /* Stamp a diamond: pixels where |dx| + |dy| <= radius. The
           visible rectangle check above only confirms the center is
           in bounds, so each diamond pixel needs its own clip.
        */
        for (int dy = -FX_DMK_RADIUS; dy <= FX_DMK_RADIUS; ++dy) {
            int span = FX_DMK_RADIUS - (dy < 0 ? -dy : dy);
            int py = y + dy;
            if (py < 0 || py >= vh)
                continue;
            for (int dx = -span; dx <= span; ++dx) {
                int px = x + dx;
                if (px < 0 || px >= vw)
                    continue;
                canvas_blend(px, py, 0xe48898a0);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Public lifecycle and per-frame                                           */
/* ------------------------------------------------------------------------ */

void jgrf_osd_set_visible(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > JGRF_OSD_W) w = JGRF_OSD_W;
    if (h > JGRF_OSD_H) h = JGRF_OSD_H;
    vis_w = w;
    vis_h = h;
}

void jgrf_osd_rehash(void) {
    if (!settings)
        return;

    switch (settings[MISC_FONT].val) {
        default:
        case 0: { // 6x8
            font = sixbyeight;
            fwidth = 6;
            fheight = 8;
            break;
        }
        case 1: { // 8x8
            font = eightbyeight;
            fwidth = 8;
            fheight = 8;
            break;
        }
    }

    jgrf_osd_toggle(osdactive);
}

void jgrf_osd_toggle(int active) {
    osdactive = active;
    if (osdactive) {
        int bgfx = settings[MISC_BGFX].val;

        // Don't reset any active effects
        if (!(bgfx >= 2 && fx_active[bgfx - 2]))
            jgrf_osd_fx_clear();

        // Reset OSD font colour
        osd_text_rgba = JGRF_OSD_ORANGE;

        // Set new shade and/or effect
        switch (bgfx) {
            case 0: default: { // None
                jgrf_osd_shade_set(0x00000000);
                break;
            }
            case 1: { // Shade
                jgrf_osd_shade_set(0xd4550040); // JG
                break;
            }
            case 2: { // Tea Swirl
                jgrf_osd_fx_enable(JGRF_OSD_FX_TEA);
                jgrf_osd_shade_set(0x6e260e80); // Oolong
                break;
            }
            case 3: { // Snow
                jgrf_osd_fx_enable(JGRF_OSD_FX_SNOW);
                jgrf_osd_shade_set(0x1010a080); // ZSNESish
                osd_text_rgba = 0xd0d0ffff; // Ice
                break;
            }
            case 4: { // Mushrooms
                jgrf_osd_fx_enable(JGRF_OSD_FX_MUSH);
                jgrf_osd_shade_set(0x000000a0); // Dark
                osd_text_rgba = 0xff2020ff; // Unapologetic red
                break;
            }
            case 5: { // Fire
                jgrf_osd_fx_enable(JGRF_OSD_FX_FIRE);
                jgrf_osd_shade_set(0x000000d0); // Very dark
                break;
            }
            case 6: { // Danmaku
                jgrf_osd_fx_enable(JGRF_OSD_FX_DMK);
                jgrf_osd_shade_set(0xa0); // Dark
                osd_text_rgba = 0x666bffff;
                break;
            }
        }
    }
    else {
        jgrf_osd_shade_set(0x00000000);
        jgrf_osd_fx_clear();
    }
}

void jgrf_osd_init(void) {
    settings = jgrf_settings_ptr();
    canvas_clear();
    shade_rgba = 0;

    for (int i = 0; i < JGRF_OSD_NUM_FX; ++i)
        fx_active[i] = 0;

    for (int i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i) {
        text_slots[i].frames = 0;
        text_slots[i].text[0] = '\0';
    }

    jgrf_osd_rehash();
}

void jgrf_osd_deinit(void) {
    jgrf_osd_fx_clear();
}

void jgrf_osd_step(void) {
    // Decrement text frame countdowns, persistent slots are left alone
    for (size_t i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i) {
        if (text_slots[i].frames > 0)
            --text_slots[i].frames;
    }
}

const void *jgrf_osd_composite(int *active, int *dirty) {
    int has_shade = shade_rgba != 0;
    int has_fx = fx_any_active();
    int has_text = text_active();

    if (active)
        *active = (has_shade || has_fx || has_text);

    if (dirty)
        *dirty = (has_shade || has_fx || has_text);

    if (!has_shade && !has_fx && !has_text)
        return canvas; // Nothing drawn, early return

    // Layer 0: Shade or clear
    if (has_shade)
        canvas_fill_visible(shade_rgba);
    else
        canvas_clear();

    // Layer 1: Effects
    if (has_fx) {
        for (size_t i = 0; i < JGRF_OSD_NUM_FX; ++i) {
            if (fx_active[i] && fx_table[i].step)
                fx_table[i].step(vis_w, vis_h);
        }
    }

    // Layer 2: Text
    if (has_text) {
        for (int i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i) {
            jgrf_osd_text_slot_t *s = &text_slots[i];
            if (s->frames == 0 || s->text[0] == '\0')
                continue;
            int tx, ty;
            resolve_anchor(s, &tx, &ty);
            jgrf_osd_render_text(tx, ty, s->rgba, s->text);
        }
    }

    return canvas;
}
