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
#include <string.h>

#include "tconfig.h"

#include "jgrf.h"
#include "settings.h"

static ini_table_s *conf;

static jgrf_gdata_t *gdata;

static size_t numemusettings = 0;
static jg_setting_t *emusettings = NULL;
static int *emusettings_default = NULL;

static size_t numsettings = 0;
static int *settings_default = NULL;

static jg_setting_t settings[] = {
    { "", "Audio: Resampler Quality", "N = Resampler Quality",
      "",
      3, 0, 10, 1
    },
    { "", "Video: API",
#ifdef JGRF_VULKAN
      "0 = OpenGL Core, 1 = OpenGL ES, 2 = OpenGL Compatibility, 3 = Vulkan",
      "",
      0, 0, 3, 1
#else
      "0 = OpenGL Core, 1 = OpenGL ES, 2 = OpenGL Compatibility",
      "",
      0, 0, 2, 1
#endif
    },
    { "", "Video: Fullscreen", "0 = Disabled, 1 = Enabled",
      "",
      0, 0, 1, 0
    },
    { "", "Video: Scale (Initial)", "N = Video Scale Factor",
      "",
      3, 1, 8, 1
    },
    { "", "Video: Shader",
      "0 = Nearest Neighbour, 1 = Linear, 2 = Sharp Bilinear, "
      "3 = Anti-Aliased Nearest Neighbour, 4 = CRT-Yee64, 5 = CRTea, 6 = LCD",
      "",
      2, 0, 6, 0
    },
    { "", "Video: CRTea Mode",
      "0 = Scanlines, 1 = Aperture Grille Lite, 2 = Aperture Grille, "
      "3 = Shadow Mask, 4 = Custom",
      "",
      2, 0, 4, 0
    },
    { "", "CRTea: Custom Mask Type",
      "0 = No Mask, 1 = Aperture Grille Lite, 2 = Aperture Grille, "
      "3 = Shadow Mask",
      "",
      2, 0, 3, 0
    },
    { "", "CRTea: Mask Strength", "N = CRTea Mask Strength",
      "",
      5, 0, 10, 0
    },
    { "", "CRTea: Scanline Strength", "N = CRTea Scanline Strength",
      "",
      6, 0, 10, 0
    },
    { "", "CRTea: Sharpness", "N = CRTea Sharpness",
      "",
      7, 0, 10, 0
    },
    { "", "CRTea: Curve", "N = CRTea Curvature",
      "",
      2, 0, 10, 0
    },
    { "", "CRTea: Corner", "N = CRTea Corner",
      "",
      3, 0, 10, 0
    },
    { "", "CRTea: Trinitron Curve", "N = CRTea Trinitron Curvature",
      "",
      10, 0, 10, 0
    },
    { "", "Input: Axis Deadzone", "N = Deadzone Size",
      "",
      5, 0, 9, 0
    },
    { "", "Cheats: Auto-activate at Boot", "0 = Disabled, 1 = Enabled",
      "",
      0, 0, 1, 1
    },
    { "", "Log Level: Core", "0 = Debug, 1 = Info, 2 = Warning, 3 = Error",
      "",
      1, 0, 3, 0
    },
    { "", "Log Level: Frontend", "0 = Debug, 1 = Info, 2 = Warning, 3 = Error",
      "",
      1, 0, 3, 0
    },
    { "", "OSD: Background Effects", "0 = None, 1 = Shade, 2 = Tea, 3 = Snow, "
      "4 = Mushrooms, 5 = Fire, 6 = Danmaku",
      "",
      2, 0, 6, 0
    },
    { "", "OSD: Font", "0 = 6x8, 1 = 8x8",
      "",
      0, 0, 1, 0
    },
};

// Read a setting with boundary check
static void jgrf_setting_rd(const char *s, const char *n, jg_setting_t *t) {
    // section, name, setting
    if (ini_table_check_entry(conf, s, n)) {
        int val_orig = t->val; // Save the default value
        ini_table_get_entry_as_int(conf, s, n, &(t->val)); // Read the value

        // Reset to default if out of bounds
        if ((t->val > t->max) || (t->val < t->min))
            t->val = val_orig;
    }
}

// Handle reading of settings
static void jgrf_settings_handler(void) {
    // Audio
    jgrf_setting_rd("audio", "rsqual", &settings[AUDIO_RSQUAL]);

    // Video
    jgrf_setting_rd("video", "api", &settings[VIDEO_API]);
    jgrf_setting_rd("video", "fullscreen", &settings[VIDEO_FULLSCREEN]);
    jgrf_setting_rd("video", "scale", &settings[VIDEO_SCALE]);
    jgrf_setting_rd("video", "shader", &settings[VIDEO_SHADER]);
    jgrf_setting_rd("video", "crtea_mode", &settings[VIDEO_CRTEA_MODE]);
    jgrf_setting_rd("video", "crtea_masktype",
        &settings[VIDEO_CRTEA_MASKTYPE]);
    jgrf_setting_rd("video", "crtea_maskstr", &settings[VIDEO_CRTEA_MASKSTR]);
    jgrf_setting_rd("video", "crtea_scanstr", &settings[VIDEO_CRTEA_SCANSTR]);
    jgrf_setting_rd("video", "crtea_sharpness",
        &settings[VIDEO_CRTEA_SHARPNESS]);
    jgrf_setting_rd("video", "crtea_curve", &settings[VIDEO_CRTEA_CURVE]);
    jgrf_setting_rd("video", "crtea_corner", &settings[VIDEO_CRTEA_CORNER]);
    jgrf_setting_rd("video", "crtea_tcurve", &settings[VIDEO_CRTEA_TCURVE]);

    // Input
    jgrf_setting_rd("input", "axis_deadzone", &settings[INPUT_AXIS_DEADZONE]);

    // Misc
    jgrf_setting_rd("misc", "cheatauto", &settings[MISC_CHEATAUTO]);
    jgrf_setting_rd("misc", "corelog", &settings[MISC_CORELOG]);
    jgrf_setting_rd("misc", "frontendlog", &settings[MISC_FRONTENDLOG]);
    jgrf_setting_rd("misc", "bgfx", &settings[MISC_BGFX]);
    jgrf_setting_rd("misc", "font", &settings[MISC_FONT]);
}

// Read the general settings ini to override defaults
static void jgrf_settings_read(void) {
    char path[192];
    snprintf(path, sizeof(path), "%ssettings.ini", gdata->configpath);

    conf = ini_table_create();

    if (!ini_table_read_from_file(conf, path))
        jgrf_log(JG_LOG_DBG, "Main configuration file not found: %s\n", path);
    else
        jgrf_settings_handler();

    // Clean up the config data
    ini_table_destroy(conf);
}

// Initialize the settings to defaults and grab global data pointer
int jgrf_settings_init(void) {
    // Grab global data pointer
    gdata = jgrf_gdata_ptr();

    // Store settings defaults to allow restoration later
    numsettings = sizeof(settings) / sizeof(jg_setting_t);
    settings_default = (int*)calloc(numsettings, sizeof(int));
    for (size_t i = 0; i < numsettings; ++i)
        settings_default[i] = settings[i].val;

    // Read settings from config file
    jgrf_settings_read();

    return 1;
}

void jgrf_settings_deinit(void) {
    if (settings_default)
        free(settings_default);
    if (emusettings_default)
        free(emusettings_default);
}

// Read core-specific overrides for frontend settings
void jgrf_settings_override(const char *name) {
    char overridepath[256];
    snprintf(overridepath, sizeof(overridepath), "%s%s.ini",
        gdata->configpath, name);

    conf = ini_table_create();

    if (!ini_table_read_from_file(conf, overridepath)) {
        jgrf_log(JG_LOG_DBG, "Override configuration file not found: %s\n",
            overridepath);
    }
    else
        jgrf_settings_handler();

    // Clean up the config data
    ini_table_destroy(conf);
}

jg_setting_t* jgrf_settings_ptr(void) {
    return settings;
}

jg_setting_t* jgrf_settings_emu_ptr(size_t *num) {
    *num = numemusettings;
    return emusettings;
}

// Read core-specific settings - "Emulator Settings"
void jgrf_settings_emu(jg_setting_t* (*get_settings)(size_t*)) {
    // Get number of settings and set local pointer to core settings array
    emusettings = get_settings(&numemusettings);

    if (!numemusettings) {
        jgrf_log(JG_LOG_DBG, "No Emulator Settings\n");
        return;
    }

    // Create an array of default settings
    emusettings_default = (int*)calloc(numemusettings, sizeof(int));

    // Build the .ini path for emulator-specific settings
    char path[256];
    snprintf(path, sizeof(path), "%s%s.ini",
        gdata->configpath, gdata->corename);

    // Create config structure
    conf = ini_table_create();
    if (!ini_table_read_from_file(conf, path))
        jgrf_log(JG_LOG_DBG, "Core configuration file not found: %s\n", path);

    for (size_t i = 0; i < numemusettings; ++i) {
        if (emusettings[i].max < emusettings[i].min) {
            jgrf_log(JG_LOG_ERR,
                "Max < Min for setting: %s\n", emusettings[i].name);
        }

        emusettings_default[i] = emusettings[i].val; // Store default setting
        if (ini_table_check_entry(conf, gdata->corename,
            emusettings[i].name)) {

            int val;
            ini_table_get_entry_as_int(conf, gdata->corename,
                emusettings[i].name, &val);

            if (val <= emusettings[i].max && val >= emusettings[i].min)
                emusettings[i].val = val;
            else
                jgrf_log(JG_LOG_WRN, "Setting out of range: %s = %d\n",
                    emusettings[i].name, val);
        }
    }

    // Clean up the config data
    ini_table_destroy(conf);
}

static void jgrf_settings_write_frontend(void) {
    char ibuf[5]; // Buffer to hold integers converted to strings

    // Audio
    snprintf(ibuf, sizeof(ibuf), "%d", settings[AUDIO_RSQUAL].val);
    ini_table_create_entry(conf, "audio", "rsqual", ibuf);

    // Video
    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_API].val);
    ini_table_create_entry(conf, "video", "api", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_FULLSCREEN].val);
    ini_table_create_entry(conf, "video", "fullscreen", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_SCALE].val);
    ini_table_create_entry(conf, "video", "scale", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_SHADER].val);
    ini_table_create_entry(conf, "video", "shader", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_MODE].val);
    ini_table_create_entry(conf, "video", "crtea_mode", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_MASKTYPE].val);
    ini_table_create_entry(conf, "video", "crtea_masktype", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_MASKSTR].val);
    ini_table_create_entry(conf, "video", "crtea_maskstr", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_SCANSTR].val);
    ini_table_create_entry(conf, "video", "crtea_scanstr", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_SHARPNESS].val);
    ini_table_create_entry(conf, "video", "crtea_sharpness", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_CURVE].val);
    ini_table_create_entry(conf, "video", "crtea_curve", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_CORNER].val);
    ini_table_create_entry(conf, "video", "crtea_corner", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[VIDEO_CRTEA_TCURVE].val);
    ini_table_create_entry(conf, "video", "crtea_tcurve", ibuf);

    // Input
    snprintf(ibuf, sizeof(ibuf), "%d", settings[INPUT_AXIS_DEADZONE].val);
    ini_table_create_entry(conf, "input", "axis_deadzone", ibuf);

    // Misc
    snprintf(ibuf, sizeof(ibuf), "%d", settings[MISC_CHEATAUTO].val);
    ini_table_create_entry(conf, "misc", "cheatauto", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[MISC_CORELOG].val);
    ini_table_create_entry(conf, "misc", "corelog", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[MISC_FRONTENDLOG].val);
    ini_table_create_entry(conf, "misc", "frontendlog", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[MISC_BGFX].val);
    ini_table_create_entry(conf, "misc", "bgfx", ibuf);

    snprintf(ibuf, sizeof(ibuf), "%d", settings[MISC_FONT].val);
    ini_table_create_entry(conf, "misc", "font", ibuf);
}

static void jgrf_settings_write_emu(void) {
    if (!numemusettings) {
        jgrf_log(JG_LOG_DBG, "No Emulator Settings\n");
        return;
    }

    char ibuf[5]; // Buffer to hold integers converted to strings

    for (size_t i = 0; i < numemusettings; ++i) {
        snprintf(ibuf, sizeof(ibuf), "%d", emusettings[i].val);
        ini_table_create_entry(conf, gdata->corename, emusettings[i].name,
            ibuf);
    }
}

void jgrf_settings_write(int opts) {
    // Create data structure
    conf = ini_table_create();
    char path[256];

    if (opts & SETTINGS_EMULATOR) { // Emulator Settings
        snprintf(path, sizeof(path), "%s%s.ini",
            gdata->configpath, gdata->corename);
        jgrf_settings_write_emu();
        if (opts & SETTINGS_FRONTEND) // Combined Settings
            jgrf_settings_write_frontend();
    }
    else if (opts & SETTINGS_FRONTEND) { // Frontend Settings
        snprintf(path, sizeof(path), "%ssettings.ini", gdata->configpath);
        jgrf_settings_write_frontend();
    }

    ini_table_write_to_file(conf, path);

    // Clean up the config data
    ini_table_destroy(conf);
}

void jgrf_settings_default(int opts) {
    if (opts & SETTINGS_FRONTEND) {
        for (size_t i = 0; i < numsettings; ++i)
            settings[i].val = settings_default[i];
        jgrf_rehash_frontend();
    }

    if (opts & SETTINGS_EMULATOR) {
        for (size_t i = 0; i < numemusettings; ++i)
            emusettings[i].val = emusettings_default[i];
        jgrf_rehash_core();
    }
}
