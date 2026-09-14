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
#include <math.h>

#include <SDL3/SDL.h>

#include "tconfig.h"

#include "jgrf.h"
#include "audio.h"
#include "cheats.h"
#include "menu.h"
#include "settings.h"
#include "video.h"
#include "input.h"
#include "osd.h"

#define MAXPORTS 12
#define MAXAXES 6
#define MAXBUTTONS 32

#define BDEADZONE 16384 // Deadzone for axes acting as buttons

#define jgrf_getticks SDL_GetTicks

// Pointers to members of core input state
typedef struct jgrf_jsmap_t {
    int16_t *axis[MAXAXES];
    uint8_t *abtn[MAXAXES * 2]; // Axes acting as buttons
    uint8_t *hatpos[4];
    uint8_t *button[MAXBUTTONS];
} jgrf_jsmap_t;

typedef struct jgrf_kbmap_t {
    uint8_t *key[SDL_SCANCODE_COUNT];
} jgrf_kbmap_t;

typedef struct jgrf_msmap_t {
    uint8_t index;
    uint8_t *button[MAXBUTTONS];
} jgrf_msmap_t;

static jg_videoinfo_t *vidinfo; // Video Info for calculating mouse input
static jg_inputinfo_t *inputinfo[MAXPORTS]; // Core Input Info
static jg_inputstate_t coreinput[MAXPORTS]; // Input states shared with core
static jg_setting_t *settings = NULL;

static jgrf_jsmap_t jsmap[MAXPORTS]; // Pointer maps for joystick/gamepad input
static unsigned rumblemap[MAXPORTS]; // Ensure force feedback lines up properly
static jgrf_kbmap_t kbmap; // Pointer map for keyboard input
static jgrf_msmap_t msmap; // Pointer map for mouse input

static jgrf_gdata_t *gdata; // Global data pointer

// SDL Joystick and Haptic pointers
static SDL_Joystick *joystick[MAXPORTS];

// Arrays to keep track of what joystick ports have a device plugged in
static int jsports[MAXPORTS];
static SDL_JoystickID jsiid[MAXPORTS]; // Joystick Instance ID

// Array to keep track of what axes are triggers vs regular "stick" axes
static unsigned trigger[MAXPORTS];

/* Bitmask of axes the gamepad mapping reports as inverted. Derived per-device
   from the mapping itself (not gated on platform), so MFi pads on macOS that
   invert Y get corrected while already-correct pads are left untouched.
*/
static unsigned axisinv[MAXPORTS];

// Undefined inputs point here - this is like /dev/null for input events
static uint8_t undef8;
static int16_t undef16;

// Configuration related globals
static ini_table_s *iconf;
static int confactive = 0;
static int confchanged = 0;
static int confindex = 0;
static jg_inputinfo_t *confiinfo = NULL; // Currently configured device
static int conf_last_axis = -1; // Last axis that was configured
static int conf_last_port = -1; // Port of last configured axis
static uint64_t conftimer = 0;
static int menuactive = 0;
static int discard_keyup = 0;

static int deadzone = 5120;

// Keyboard Mode
static int kbmode = 0; // Default to Keyboard Mode (hotkeys disabled) off

void jgrf_input_config_enable(int e) {
    confactive = e;
}

void jgrf_input_menu_enable(int e) {
    menuactive = e;
    if (!jgrf_get_paused())
        jgrf_osd_toggle(menuactive);
}

// Map a core input axis definition
void jgrf_input_map_axis(int index, uint32_t dnum, const char* value) {
    //printf("axis: %d, %d, %s\n", index, dnum, value);
    if (value[0] == 'j') { // Joystick
        uint8_t inum = value[1] - '0'; // Index (Frontend)
        if (value[2] == 'a') { // Axis
            int anum = atoi(&value[3]);
            jsmap[inum].axis[anum] = &(coreinput[index].axis[dnum]);
            rumblemap[inum] = index; // Synchronize force feedback with axes
        }
    }

    conftimer = jgrf_getticks();
}

// Map a core input button definition
void jgrf_input_map_button(int index, uint32_t dnum, const char* value) {
    //printf("button: %d, %d, %s\n", index, dnum, value);
    if (value[0] == 'j') { // Joystick
        uint8_t inum = value[1] - '0'; // Index (Frontend)
        if (value[2] == 'a') { // Axis acting as Button
            int anum = atoi(&value[3]) * 2;
            if (value[4] == '+')
                anum++;
            jsmap[inum].abtn[anum] = &(coreinput[index].button[dnum]);
        }
        else if (value[2] == 'b') { // Button
            int bnum = atoi(&value[3]);
            jsmap[inum].button[bnum] = &(coreinput[index].button[dnum]);
        }
        else if (value[2] == 'h') { // Hat switch
            uint8_t hinum = value[1] - '0'; // Index (Frontend)
            // Assume hat switch 0 - are there devices with more than one?
            jsmap[hinum].hatpos[value[4] - '0'] =
                &(coreinput[index].button[dnum]);
        }
    }
    else if (value[0] == 'm') { // Mouse
        if (value[1] == 'b') {
            int bnum = atoi(&value[2]);
            msmap.index = index;
            msmap.button[bnum] = &(coreinput[index].button[dnum]);
        }
    }
    else { // Keyboard
        int knum = atoi(value);
        kbmap.key[knum] = &(coreinput[index].button[dnum]);
        *kbmap.key[knum] = 0;
    }

    conftimer = jgrf_getticks();
}

// Read an input config file and assign inputs
static void jgrf_inputcfg_read(jg_inputinfo_t *iinfo) {
    char path[256];
    snprintf(path, sizeof(path), "%s%s_input.ini",
        gdata->configpath, gdata->corename);

    for (int i = 0; i < iinfo->numaxes; ++i) {
        if (ini_table_check_entry(iconf, iinfo->name, iinfo->defs[i])) {
            jgrf_input_map_axis(iinfo->index, i,
                ini_table_get_entry(iconf, iinfo->name, iinfo->defs[i]));
        }
    }

    for (int i = 0; i < iinfo->numbuttons; ++i) {
        if (ini_table_check_entry(iconf, iinfo->name,
            iinfo->defs[i + iinfo->numaxes])) {
            jgrf_input_map_button(iinfo->index, i,
                ini_table_get_entry(iconf, iinfo->name,
                iinfo->defs[i + iinfo->numaxes]));
        }
    }
}

// Unmap/undefine all joystick map pointers
static void jgrf_input_undef_port(int port) {
    for (int j = 0; j < MAXAXES; ++j) {
        jsmap[port].axis[j] = &undef16;
        jsmap[port].abtn[j * 2] = jsmap[port].abtn[(j * 2) + 1] = &undef8;
    }

    for (int j = 0; j < 4; ++j)
        jsmap[port].hatpos[j] = &undef8;

    for (int j = 0; j < MAXBUTTONS; ++j)
        jsmap[port].button[j] = &undef8;
}

// Set all joystick mappings to undefined
void jgrf_input_undef(void) {
    for (int i = 0; i < MAXPORTS; ++i) {
        jgrf_input_undef_port(i);
    }

    // Set all keyboard mappings to undefined
    for (int j = 0; j < SDL_SCANCODE_COUNT; ++j)
        kbmap.key[j] = &undef8;

    // Set all mouse mappings to undefined
    for (int j = 0; j < MAXBUTTONS; ++j)
        msmap.button[j] = &undef8;
}

// Re-apply bindings to all currently plugged in devices
static void jgrf_input_rebind(void) {
    gdata = jgrf_gdata_ptr();

    // Undefine all live mappings
    jgrf_input_undef();

    // Re-read bindings for every plugged-in device
    for (int i = 0; i < gdata->numinputs; ++i) {
        if (inputinfo[i] && inputinfo[i]->name) {
            jgrf_inputcfg_read(inputinfo[i]);
        }
    }
}

/* Resolve a device's port from its instance id. Track this mapping manually
   rather than reading SDL_GetJoystickPlayerIndex(), because on macOS the
   GameController framework owns the player index and reassigns it
   asynchronously after a device is opened, which misroutes or drops input
   events.
*/
static int jgrf_input_port(SDL_JoystickID which) {
    for (int i = 0; i < MAXPORTS; ++i) {
        if (jsports[i] && jsiid[i] == which)
            return i;
    }
    return -1;
}

// Initialize input
int jgrf_input_init(void) {
    // Grab pointer to settings
    settings = jgrf_settings_ptr();

    // Set the axis deadzone
    deadzone = settings[INPUT_AXIS_DEADZONE].val * 1024;

    jgrf_input_undef();

    // Initialize the input configuration structure
    char path[256];
    snprintf(path, sizeof(path), "%s%s_input.ini",
        gdata->configpath, gdata->corename);

    iconf = ini_table_create();
    if (!ini_table_read_from_file(iconf, path))
        jgrf_log(JG_LOG_WRN, "Input configuration file not found: %s\n", path);

    // Set pointer to video info - Move coordinate computation?
    vidinfo = jgrf_video_get_info();

    return 1;
}

// Deinitialize input and save changes if necessary
void jgrf_input_deinit(void) {
    // Deinitialize joysticks
    int numjs = 0;
    SDL_JoystickID *jsids = SDL_GetJoysticks(&numjs);
    if (jsids)
        SDL_free(jsids);

    for (int i = 0; i < MAXPORTS; ++i) {
        if (joystick[i]) {
            SDL_CloseJoystick(joystick[i]);
            joystick[i] = NULL;
        }
    }

    jgrf_input_deinit_core();

    // Write out the input config file
    if (confchanged) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s_input.ini",
            gdata->configpath, gdata->corename);

        ini_table_write_to_file(iconf, path);
    }

    // Clean up the input config data
    ini_table_destroy(iconf);
}

// Free memory allocated for core input states
void jgrf_input_deinit_core(void) {
    for (int i = 0; i < MAXPORTS; ++i) {
        if (coreinput[i].axis)
            free(coreinput[i].axis);
        if (coreinput[i].button)
            free(coreinput[i].button);
        if (coreinput[i].coord)
            free(coreinput[i].coord);
        if (coreinput[i].rel)
            free(coreinput[i].rel);
    }
}

// Rehash input
void jgrf_input_rehash(void) {
    // Grab pointer to settings
    settings = jgrf_settings_ptr();
    deadzone = settings[INPUT_AXIS_DEADZONE].val * 1024;
}

// Handle joystick hotplug additions
static void jgrf_input_hotplug_add(SDL_Event *event) {
    SDL_JoystickID which = event->jdevice.which;
    int port = 0;

    /* Ignore if this device is already tracked. Startup enumeration and the
       queued ADDED event can reference the same device. Without this guard, it
       would be opened twice and occupy two ports.
    */
    if (jgrf_input_port(which) != -1)
        return;

    for (int i = 0; i < MAXPORTS; ++i) {
        if (!jsports[i]) {
            joystick[i] = SDL_OpenJoystick(which);
            if (!joystick[i]) { // Opening can fail (e.g. macOS permissions)
                jgrf_log(JG_LOG_WRN, "Failed to open joystick: %s\n",
                    SDL_GetError());
                return;
            }
            SDL_SetJoystickPlayerIndex(joystick[i], i);
            jsports[i] = 1;
            jsiid[i] = SDL_GetJoystickID(joystick[i]);
            port = i;

            jgrf_log(JG_LOG_INF, "Joystick %d Connected: %s\n",
                port + 1, SDL_GetJoystickName(joystick[port]));

            if (SDL_IsGamepad(event->jdevice.which)) {
                trigger[i] = 0;
                axisinv[i] = 0;

                SDL_Gamepad *gp = SDL_OpenGamepad(event->jdevice.which);

                int nbindings = 0;
                SDL_GamepadBinding **bindings =
                    SDL_GetGamepadBindings(gp, &nbindings);

                if (bindings) {
                    for (int b = 0; b < nbindings; ++b) {
                        if (bindings[b]->output_type ==
                            SDL_GAMEPAD_BINDTYPE_AXIS) {
                            if (bindings[b]->output.axis.axis ==
                                SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
                                bindings[b]->input_type ==
                                SDL_GAMEPAD_BINDTYPE_AXIS) {
                                trigger[port] |=
                                    1 << bindings[b]->input.axis.axis;
                            }
                            if (bindings[b]->output.axis.axis ==
                                SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
                                bindings[b]->input_type ==
                                SDL_GAMEPAD_BINDTYPE_AXIS) {
                                trigger[port] |=
                                    1 << bindings[b]->input.axis.axis;
                            }
                            /* Honour inverted stick axes as declared by the
                               gamepad mapping. An axis is inverted when its
                               input and output ranges run in opposite
                               directions (one ascending, one descending). This
                               is how MFi controllers on macOS surface their
                               inverted Y to the raw joystick layer. Triggers
                               are excluded - they are handled above.
                            */
                            if (bindings[b]->input_type ==
                                SDL_GAMEPAD_BINDTYPE_AXIS &&
                                bindings[b]->output.axis.axis !=
                                SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
                                bindings[b]->output.axis.axis !=
                                SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                                int in_desc = bindings[b]->input.axis.axis_min >
                                    bindings[b]->input.axis.axis_max;
                                int out_desc =
                                    bindings[b]->output.axis.axis_min >
                                    bindings[b]->output.axis.axis_max;
                                if (in_desc != out_desc) {
                                    axisinv[port] |=
                                        1 << bindings[b]->input.axis.axis;
                                    jgrf_log(JG_LOG_DBG, "Axis %d inverted\n",
                                        bindings[b]->input.axis.axis);
                                }
                            }
                        }
                    }
                    SDL_free(bindings);
                }

                SDL_CloseGamepad(gp);
            }

            // Normalize initial axis values: minimum for trigger, 0 for center
            int numaxes = SDL_GetNumJoystickAxes(joystick[i]);
            for (int j = 0; j < numaxes; ++j)
                *jsmap[i].axis[j] = (trigger[i] & (1 << j)) ? -32768 : 0;
            break;
        }
    }
}

// Handle joystick hotplug removals
static void jgrf_input_hotplug_remove(SDL_Event *event) {
    for (int i = 0; i < MAXPORTS; ++i) {
        // If it's the one that got disconnected...
        if (jsiid[i] == event->jdevice.which) {
            jsports[i] = 0; // This is unplugged
            jgrf_log(JG_LOG_INF, "Joystick %d Disconnected\n", i + 1);
            SDL_CloseJoystick(joystick[i]);
            joystick[i] = NULL;
            jsports[i] = 0;
            break;
        }
    }
}

// Retrieve inputinfo data so the frontend knows what the core has plugged in
void jgrf_input_query(jg_inputinfo_t* (*get_inputinfo)(int)) {
    // Make sure cursor state is defaulted
    SDL_HideCursor();
    SDL_SetCursor(NULL);

    for (int i = 0; i < gdata->numinputs; ++i) {
        inputinfo[i] = get_inputinfo(i);

        if (inputinfo[i]->name) {
            jgrf_log(JG_LOG_INF,
                "Emulated Input %d: %s, %s, %d axes, %d buttons\n",
                i + 1, inputinfo[i]->name, inputinfo[i]->fname,
                inputinfo[i]->numaxes, inputinfo[i]->numbuttons);

            // Allocate memory for the emulated buttons/axes/coords
            coreinput[i].axis =
                (int16_t*)calloc(inputinfo[i]->numaxes, sizeof(int16_t));
            coreinput[i].button =
                (uint8_t*)calloc(inputinfo[i]->numbuttons, sizeof(uint8_t));
            coreinput[i].coord = // There are always X, Y, and Z coords
                (int32_t*)calloc(3, sizeof(int32_t)); // Magic Number
            coreinput[i].rel = // There is always X and Y relative motion
                (int32_t*)calloc(2, sizeof(int32_t)); // Magic Number

            // Read configuration for this emulated device
            jgrf_inputcfg_read(inputinfo[i]);
        }

        if (inputinfo[i]->type == JG_INPUT_GUN)
            jgrf_video_set_cursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        else if (inputinfo[i]->type == JG_INPUT_TOUCH)
            jgrf_video_set_cursor(SDL_SYSTEM_CURSOR_POINTER);
    }
}

// Pass pointers to input states into the core
void jgrf_input_set_states(void (*set_inputstate)(jg_inputstate_t*, int)) {
    gdata = jgrf_gdata_ptr();
    for(int i = 0; i < gdata->numinputs; ++i)
        set_inputstate(&coreinput[i], i);
}

// Calculate mouse input coordinates, taking into account aspect ratio and scale
static inline void jgrf_input_coords_scaled(float x, float y,
    int32_t *xcoord, int32_t *ycoord) {

    float xscale, yscale, xo, yo;
    jgrf_video_get_scale_params(&xscale, &yscale, &xo, &yo);
    *xcoord = (x - xo) /
        ((vidinfo->aspect * vidinfo->h * xscale)/(float)vidinfo->w);
    *ycoord = ((y - yo) / yscale) + vidinfo->y;
}

// Run an input configuration iteration to set up a specific definition
static void jgrf_inputcfg(jg_inputinfo_t *iinfo) {
    if (confindex >= (iinfo->numaxes + iinfo->numbuttons)) {
        confactive = 0; // Turn off input config mode
        confindex = 0;
        jgrf_video_text(2, 0, ""); // Disable display of input config info
        jgrf_input_rebind(); // Apply newly saved bindings to live devices
        if (menuactive)
            jgrf_menu_text_redraw(); // Turn the menu text back on
        return;
    }
    else {
        confactive = 1;
        conftimer = jgrf_getticks();
    }

    // Display input config information on screen
    if (iinfo->name) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "%s\n%s\n%d axes, %d buttons\nConfigure %s",
            iinfo->fname, iinfo->name,
            iinfo->numaxes, iinfo->numbuttons, iinfo->defs[confindex]);
        jgrf_video_text(2, 1, msg);
    }
}

// Create config definitions from input events, then configure them to be used
static void jgrf_inputcfg_handler(SDL_Event *event) {
    char defbuf[32];

    int axis_configured = -1;

    switch(event->type) {
        case SDL_EVENT_KEY_UP: {
            if (discard_keyup) {
                --discard_keyup;
                break;
            }

            if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
                ini_table_create_entry(iconf, confiinfo->name,
                    confiinfo->defs[confindex], "");
                confindex++;
                jgrf_inputcfg(confiinfo);
                break;
            }

            if (event->key.repeat)
                break;

            if (confindex < confiinfo->numaxes) {
                jgrf_log(JG_LOG_WRN, "Trying to assign digital inputs to axes"
                    " is a losing endeavour. ESC to skip.\n");
                jgrf_log(JG_LOG_SCR, "Analog input required");
                break;
            }

            snprintf(defbuf, sizeof(defbuf), "%d",
                event->key.scancode);

            ini_table_create_entry(iconf, confiinfo->name,
                confiinfo->defs[confindex], defbuf);

            confindex++;
            jgrf_inputcfg(confiinfo);
            break;
        }
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN: {
            if (confindex < confiinfo->numaxes) {
                jgrf_log(JG_LOG_WRN, "Trying to assign digital inputs to axes"
                    " is a losing endeavour. ESC to skip.\n");
                jgrf_log(JG_LOG_SCR, "Analog input required");
                break;
            }

            int port = jgrf_input_port(event->jbutton.which);
            if (port < 0)
                break;

            snprintf(defbuf, sizeof(defbuf), "j%db%d",
                port, event->jbutton.button);

            ini_table_create_entry(iconf, confiinfo->name,
                confiinfo->defs[confindex], defbuf);

            confindex++;
            jgrf_inputcfg(confiinfo);
            break;
        }
        case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
            int port = jgrf_input_port(event->jaxis.which);
            if (port < 0)
                break;

            // Triggers require special handling
            if (trigger[port] & (1 << event->jaxis.axis)) {
                // Axes set to axis input
                if (confindex < confiinfo->numaxes) {
                    if (event->jaxis.value >= BDEADZONE) {
                        snprintf(defbuf, sizeof(defbuf), "j%da%d",
                            port, event->jaxis.axis);

                        ini_table_create_entry(iconf, confiinfo->name,
                            confiinfo->defs[confindex], defbuf);

                        axis_configured = port;
                    }
                }
                else { // Axes set to button input
                    if (event->jaxis.value >= BDEADZONE) {
                        snprintf(defbuf, sizeof(defbuf), "j%da%d+",
                            port, event->jaxis.axis);

                        ini_table_create_entry(iconf, confiinfo->name,
                            confiinfo->defs[confindex], defbuf);

                        axis_configured = port;
                    }
                }
            }
            else { // Normal axis
                // Axes set to axis input
                if (confindex < confiinfo->numaxes) {
                    if (abs(event->jaxis.value) >= BDEADZONE) {
                        snprintf(defbuf, sizeof(defbuf), "j%da%d",
                            port, event->jaxis.axis);

                        ini_table_create_entry(iconf, confiinfo->name,
                            confiinfo->defs[confindex], defbuf);

                        axis_configured = port;
                    }
                }
                else { // Axes set to button input
                    if (abs(event->jaxis.value) >= 32767) {
                        // Handle the case of hat switches pretending to be axes
                        snprintf(defbuf, sizeof(defbuf), "j%da%d%c",
                            port, event->jaxis.axis,
                            event->jaxis.value > 0 ? '+' : '-');

                        ini_table_create_entry(iconf, confiinfo->name,
                            confiinfo->defs[confindex], defbuf);

                        axis_configured = port;
                        break;
                    }
                    if (abs(event->jaxis.value) >= BDEADZONE) {
                        snprintf(defbuf, sizeof(defbuf), "j%da%d%c",
                            port, event->jaxis.axis,
                            event->jaxis.value > 0 ? '+' : '-');

                        ini_table_create_entry(iconf, confiinfo->name,
                            confiinfo->defs[confindex], defbuf);

                        axis_configured = port;
                    }
                }
            }
            break;
        }
        case SDL_EVENT_JOYSTICK_HAT_MOTION: {
            if (confindex < confiinfo->numaxes) {
                jgrf_log(JG_LOG_WRN, "Trying to assign digital inputs to axes"
                    " is a losing endeavour. ESC to skip.\n");
                jgrf_log(JG_LOG_SCR, "Analog input required");
                break;
            }

            int port = jgrf_input_port(event->jhat.which);
            if (port < 0)
                break;

            if (event->jhat.value & SDL_HAT_UP)
                snprintf(defbuf, sizeof(defbuf), "j%dh00", port);

            else if (event->jhat.value & SDL_HAT_DOWN)
                snprintf(defbuf, sizeof(defbuf), "j%dh01", port);

            else if (event->jhat.value & SDL_HAT_LEFT)
                snprintf(defbuf, sizeof(defbuf), "j%dh02", port);

            else if (event->jhat.value & SDL_HAT_RIGHT)
                snprintf(defbuf, sizeof(defbuf), "j%dh03", port);

            if (event->jhat.value != 0) {
                ini_table_create_entry(iconf, confiinfo->name,
                    confiinfo->defs[confindex], defbuf);

                confindex++;
                jgrf_inputcfg(confiinfo);
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (confindex < confiinfo->numaxes)
                break;

            snprintf(defbuf, sizeof(defbuf), "mb%d", event->button.button);

            ini_table_create_entry(iconf, confiinfo->name,
                confiinfo->defs[confindex], defbuf);

            confindex++;
            jgrf_inputcfg(confiinfo);
            break;
        }
        case SDL_EVENT_JOYSTICK_ADDED: {
            jgrf_input_hotplug_add(event);
            break;
        }
        case SDL_EVENT_JOYSTICK_REMOVED: {
            jgrf_input_hotplug_remove(event);
            break;
        }
        default: {
            break;
        }
    }

    if (axis_configured != -1) {
        confindex++;
        jgrf_inputcfg(confiinfo);
        conf_last_axis = event->jaxis.axis;
        conf_last_port = axis_configured;
    }
}

// Main input event handler
void jgrf_input_handler(SDL_Event *event) {
    /* Correct inverted stick axes at the source (e.g. MFi Y on macOS), once,
       before any branch consumes the event. This keeps both input
       configuration and gameplay reading SDL-convention values. The event is
       our own local copy, so mutating it here is safe.
    */
    if (event->type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        int port = jgrf_input_port(event->jaxis.which);
        if (port >= 0 && (axisinv[port] & (1 << event->jaxis.axis)))
            event->jaxis.value = (event->jaxis.value == INT16_MIN)
                ? INT16_MAX : -event->jaxis.value;
    }

    if (confactive) {
        unsigned delay = 60; // Delay consecutive inputs by 60 ticks
        if (event->type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
            /* In some cases, there is an extra axis which takes input from
               two other axes (triggers), which shows up in the SDL event queue
               before the legitimate axes. This is a hack to ignore events on
               the extra axis, which will have an even index, since we count
               from 0 and axes typically come in pairs.
               Reference: https://github.com/atar-axis/xpadneo/issues/334
            */
            int port = jgrf_input_port(event->jaxis.which);
            if (port < 0)
                return;

            // If this is the axis we just configured, ignore until neutral
            if (port == conf_last_port && event->jaxis.axis == conf_last_axis) {
                if (abs(event->jaxis.value) < BDEADZONE) {
                    // Axis returned to neutral, clear the lock
                    conf_last_axis = -1;
                    conf_last_port = -1;
                }
                return; // Ignore this event either way
            }

            int extra = SDL_GetNumJoystickAxes(joystick[port]) - 1;
            if ((event->jaxis.axis == extra) && !(extra & 1))
                return;

            delay = 420; // Larger delay required for axes
        }

        // Determine ticks since the last input definition was configured
        uint64_t delta = jgrf_getticks() - conftimer;

        // If the delta is large enough, pass the event to input config
        if (delta > delay)
            jgrf_inputcfg_handler(event);
        return;
    }
    else if (menuactive) {
        switch (event->type) {
            case SDL_EVENT_KEY_UP: jgrf_menu_input_handler(event); break;
            case SDL_EVENT_JOYSTICK_ADDED:
                jgrf_input_hotplug_add(event);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                jgrf_input_hotplug_remove(event);
                break;
            default: break;
        }
        return;
    }

    // Check if we need to change keyboard mode
    // SDL3: event->key.mod replaces event->key.keysym.mod
    // SDL3: event->key.scancode replaces event->key.keysym.scancode
    if ((event->key.mod & SDL_KMOD_SHIFT) &&
        (event->key.scancode == SDL_SCANCODE_TAB)) {
        if (event->type == SDL_EVENT_KEY_UP) {
            kbmode ^= 1;
            jgrf_log(JG_LOG_SCR, "Hotkeys %s", kbmode ? "Disabled" : "Enabled");
        }
        return;
    }

    // This needs to be fixed and worked into the rest of the system one day...
    if (!kbmode && (event->type == SDL_EVENT_KEY_UP ||
        event->type == SDL_EVENT_KEY_DOWN)) {
        switch (event->key.scancode) {
            case SDL_SCANCODE_ESCAPE: {
                jgrf_schedule_quit();
                break;
            }
            case SDL_SCANCODE_GRAVE: {
                jgrf_set_speed(event->type == SDL_EVENT_KEY_DOWN ? 1 : 0);
                break;
            }
            case SDL_SCANCODE_F1: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_reset(0);
                break;
            }
            case SDL_SCANCODE_F2: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_reset(1);
                break;
            }
            case SDL_SCANCODE_F5: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_state_save(0);
                break;
            }
            case SDL_SCANCODE_F6: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_state_save(1);
                break;
            }
            case SDL_SCANCODE_F7: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_state_load(0);
                break;
            }
            case SDL_SCANCODE_F8: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_state_load(1);
                break;
            }
            case SDL_SCANCODE_F9: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_video_screenshot();
                break;
            }
            case SDL_SCANCODE_F12: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_cheats_toggle();
                break;
            }
            case SDL_SCANCODE_F: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_video_fullscreen();
                break;
            }
            case SDL_SCANCODE_M: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_audio_toggle();
                break;
            }
            case SDL_SCANCODE_P: {
                if (event->type == SDL_EVENT_KEY_UP) jgrf_pause_toggle();
                break;
            }
            case SDL_SCANCODE_1:
            case SDL_SCANCODE_2:
            case SDL_SCANCODE_3:
            case SDL_SCANCODE_4:
            case SDL_SCANCODE_5:
            case SDL_SCANCODE_6:
            case SDL_SCANCODE_7:
            case SDL_SCANCODE_8: {
            // Want to play 12-player sports games? Hand-edit the config file.
                if (event->type == SDL_EVENT_KEY_UP &&
                    (event->key.mod & SDL_KMOD_SHIFT)) {
                    int port =
                        atoi(SDL_GetScancodeName(event->key.scancode));

                    if (port > gdata->numinputs || port < 1)
                        break;

                    port--;

                    /* Discard a keyup event to prevent it from registering as
                       a desired input definition
                    */
                    discard_keyup = 1;

                    jgrf_input_config(inputinfo[port]);
                }
                break;
            }
            case SDL_SCANCODE_TAB: {
                if (event->type == SDL_EVENT_KEY_UP) {
                    jgrf_input_menu_enable(1);
                    jgrf_menu_display();
                }
                return;
            }
            default: {
                break;
            }
        }
    }

    // Game input events
    switch(event->type) {
        case SDL_EVENT_KEY_UP: {
            *kbmap.key[event->key.scancode] = 0;
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            *kbmap.key[event->key.scancode] = 1;
            break;
        }
        case SDL_EVENT_JOYSTICK_BUTTON_UP: {
            int port = jgrf_input_port(event->jbutton.which);
            if (port < 0)
                break;
            *jsmap[port].button[event->jbutton.button] = 0;
            break;
        }
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN: {
            int port = jgrf_input_port(event->jbutton.which);
            if (port < 0)
                break;
            *jsmap[port].button[event->jbutton.button] = 1;
            break;
        }
        case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
            int port = jgrf_input_port(event->jaxis.which);
            if (port < 0)
                break;
            // Value is already corrected for inversion at the handler entry.
            *jsmap[port].axis[event->jaxis.axis] =
                abs(event->jaxis.value) > deadzone ? event->jaxis.value : 0;

            if (abs(event->jaxis.value) > BDEADZONE) {
                *jsmap[port].abtn[event->jaxis.axis * 2] =
                    event->jaxis.value < 0;
                *jsmap[port].abtn[(event->jaxis.axis * 2) + 1] =
                    event->jaxis.value > 0;
            }
            else {
                *jsmap[port].abtn[event->jaxis.axis * 2] = 0;
                *jsmap[port].abtn[(event->jaxis.axis * 2) + 1] = 0;
            }
            break;
        }
        case SDL_EVENT_JOYSTICK_HAT_MOTION: {
            int port = jgrf_input_port(event->jhat.which);
            if (port < 0)
                break;
            *jsmap[port].hatpos[0] = event->jhat.value & SDL_HAT_UP;
            *jsmap[port].hatpos[1] = (event->jhat.value & SDL_HAT_DOWN) >> 2;
            *jsmap[port].hatpos[2] = (event->jhat.value & SDL_HAT_LEFT) >> 3;
            *jsmap[port].hatpos[3] = (event->jhat.value & SDL_HAT_RIGHT) >> 1;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            jgrf_input_coords_scaled(event->motion.x, event->motion.y,
                &coreinput[msmap.index].coord[0],
                &coreinput[msmap.index].coord[1]);
            coreinput[msmap.index].rel[0] += event->motion.xrel;
            coreinput[msmap.index].rel[1] += event->motion.yrel;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            *msmap.button[event->button.button] = 0;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            *msmap.button[event->button.button] = 1;
            break;
        }
        case SDL_EVENT_JOYSTICK_ADDED: {
            jgrf_input_hotplug_add(event);
            break;
        }
        case SDL_EVENT_JOYSTICK_REMOVED: {
            jgrf_input_hotplug_remove(event);
            break;
        }
        default: {
            break;
        }
    }
}

// Callback used by core to rumble the physical input device
void jgrf_input_rumble(void *udata, int port, float strength, size_t len) {
    (void)udata;
    uint16_t str = (uint16_t)(strength * 0xFFFF);
    if (len == 0)
        SDL_RumbleJoystick(joystick[rumblemap[port]], 0, 0, 0);
    else
        SDL_RumbleJoystick(joystick[rumblemap[port]], str, str, len);
}

void jgrf_input_config(jg_inputinfo_t *iinfo) {
    if (!iinfo || !iinfo->name)
        return;

    confiinfo = iinfo;
    confindex = 0;
    confchanged = 1;
    jgrf_inputcfg(confiinfo);
}
