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

#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTINGS_FRONTEND 0x01
#define SETTINGS_EMULATOR 0x02

enum _jgrf_settings {
    AUDIO_RSQUAL,
    VIDEO_API,
    VIDEO_FULLSCREEN,
    VIDEO_SCALE,
    VIDEO_SHADER,
    VIDEO_CRTEA_MODE,
    VIDEO_CRTEA_MASKTYPE,
    VIDEO_CRTEA_MASKSTR,
    VIDEO_CRTEA_SCANSTR,
    VIDEO_CRTEA_SHARPNESS,
    VIDEO_CRTEA_CURVE,
    VIDEO_CRTEA_CORNER,
    VIDEO_CRTEA_TCURVE,
    INPUT_AXIS_DEADZONE,
    MISC_CHEATAUTO,
    MISC_CORELOG,
    MISC_FRONTENDLOG,
    MISC_BGFX,
    MISC_FONT,
    JGRF_SETTINGS_MAX
};

int jgrf_settings_init(void);
void jgrf_settings_deinit(void);
jg_setting_t* jgrf_settings_ptr(void);
jg_setting_t* jgrf_settings_emu_ptr(size_t*);
void jgrf_settings_emu(jg_setting_t* (*)(size_t*));
void jgrf_settings_override(const char *);
void jgrf_settings_write(int);
void jgrf_settings_default(int);

#endif
