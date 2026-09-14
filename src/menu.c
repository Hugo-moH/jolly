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

#ifndef __APPLE__
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "ezmenu.h"

#include "jgrf.h"
#include "input.h"
#include "settings.h"
#include "dips.h"
#include "video.h"
#include "menu.h"

#define DESCSIZE 256
#define NUMLINES 256
#define TEXTSIZE 1024

enum _menumode {
    FRONTEND,
    EMULATOR,
    DIPS,
    INPUT,
    MEDIA,
    SAVESETTINGS
};

typedef struct _menunode_t {
    struct _menunode_t *parent;
    struct _menunode_t *child;
    struct _menunode_t *next;
    char desc[DESCSIZE];
    int val;
    int mode;
} menunode_t;

// ezmenu context
static struct ezmenu ezm;

// Menu can remember its path in the menu 4 levels deep (8 bits per level)
static uint32_t menupath = 0;

// Pointer to the core's video information
static jg_videoinfo_t *vidinfo = NULL;

static jg_setting_t *settings = NULL;
static jg_setting_t *emusettings = NULL;
static size_t numemusettings = 0;

static jg_setting_t *dips = NULL;
static size_t numdips = 0;

static jg_mediainfo_t *mediainfo = NULL;

static menunode_t *menuroot = NULL;
static menunode_t *menulevel = NULL;

static jg_inputinfo_t *inputlist = NULL;
static size_t inputlist_count = 0;

static char textbuf[TEXTSIZE]; // Buffer for all text on screen
static char *linebuf[NUMLINES]; // 64 lines of 255 + '\0' characters

static int menumode = 0;
static int settinglevel = 0;
static int settingactive = 0;
static int skip_redraw = 0;

// Concatenate two strings in a Jolly Good manner
static void strjcat(char *dest, const char *src, size_t n) {
    size_t i = 0;
    size_t dlen = strlen(dest);
    size_t slen = strlen(src);

    if (!n)
        return;

    if (dlen + slen > (n - 1))
        slen = (n - 1) - dlen;

    for (i = 0; i < slen; ++i)
        dest[i + (dlen)] = src[i];
    dest[i + dlen] = '\0';
}

static menunode_t* jgrf_menu_node_create(void) {
    return (menunode_t*)calloc(1, sizeof(menunode_t));
}

// Add a child node to another node, or a sibling if there is already a child
static menunode_t* jgrf_menu_node_add_child(menunode_t *parent) {
    // Create the new node and assign the pointer to its parent
    menunode_t *newnode = jgrf_menu_node_create();
    newnode->parent = parent;

    // If the parent node already had a child, add a sibling instead
    if (parent->child) {
        menunode_t *current = parent->child;
        while (current->next)
            current = current->next;
        current->next = newnode;
    }
    else {
        parent->child = newnode;
    }

    return newnode;
}

// Free a node and every node to the right or below it
static void jgrf_menu_node_free(menunode_t *node) {
    if (node->next)
        jgrf_menu_node_free(node->next);

    if (node->child)
        jgrf_menu_node_free(node->child);

    free(node);
    node = NULL;
}

// Generate the nodes for a settings subtree
static void jgrf_menu_gen(menunode_t *node, jg_setting_t *s, unsigned num) {
    // Generate the text to be displayed
    for (unsigned i = 0; i < num; ++i) {
        // Create a menu item node
        menunode_t *menuitem = jgrf_menu_node_add_child(node);
        snprintf(menuitem->desc, DESCSIZE, "%s", s[i].fname);
        menuitem->val = i;

        // Add child nodes containing the settings for each menu item
        char buf[DESCSIZE];
        snprintf(buf, DESCSIZE, "%s", s[i].opts);
        char *bufptr;
        char *token = strtok_r(buf, ",", &bufptr);

        while (token != NULL) {
            while (*token == ' ')
                ++token;

            if (token[0] == 'N') { // Special case for numerical ranges
                unsigned range = abs(s[i].max - s[i].min) + 1;
                for (unsigned j = 0; j < range; ++j) {
                    menunode_t *setting = jgrf_menu_node_add_child(menuitem);
                    snprintf(setting->desc, DESCSIZE, "%d", j + s[i].min);
                    setting->val = j + s[i].min;
                }
            }
            else { // Regular nodes
                menunode_t *setting = jgrf_menu_node_add_child(menuitem);
                snprintf(setting->desc, DESCSIZE, "%s", token);
                char vbuf[DESCSIZE];
                snprintf(vbuf, DESCSIZE, "%s", token);
                char *vbufptr;
                char *vtoken = strtok_r(vbuf, " =", &vbufptr);
                setting->val = atoi(vtoken);
            }
            token = strtok_r(NULL, ",", &bufptr);
        }
    }
}

// Generate the nodes for Input Mapping
static void jgrf_menu_gen_input(menunode_t *node) {
    inputlist = jgrf_jgapi_ptr()->jg_get_inputlist(&inputlist_count);

    if (!inputlist)
        return;

    /* Walk the slot list grouping consecutive slots by fname. Singleton
       families produce a single configurable node. Multi-instance families
       produce a parent node with one child per instance.
    */
    size_t i = 0;
    while (i < inputlist_count) {
        if (inputlist[i].name == NULL || inputlist[i].fname == NULL) {
            ++i;
            continue;
        }

        // Determine the extent of the current family
        size_t j = i + 1;
        while (j < inputlist_count && inputlist[j].fname != NULL &&
            strcmp(inputlist[j].fname, inputlist[i].fname) == 0) {
            ++j;
        }

        size_t family_size = j - i;
        menunode_t *family = jgrf_menu_node_add_child(node);
        snprintf(family->desc, DESCSIZE, "%s", inputlist[i].fname);

        if (family_size == 1) {
            // Singleton family: configure directly when selected
            family->val = (int)i;
        }
        else {
            // Multi-instance family: build a submenu of instances
            family->val = -1;
            for (size_t k = i; k < j; ++k) {
                menunode_t *instance = jgrf_menu_node_add_child(family);
                snprintf(instance->desc, DESCSIZE, "%s %d",
                    inputlist[k].fname, inputlist[k].index + 1);
                instance->val = (int)k;
            }
        }

        i = j;
    }
}

// Generate the nodes for Media selection
static void jgrf_menu_gen_media(menunode_t *node) {
    if (!mediainfo) return;
    for (unsigned i = 0; i < mediainfo->count; ++i) {
        menunode_t *entry = jgrf_menu_node_add_child(node);
        snprintf(entry->desc, DESCSIZE, "%s %u", mediainfo->name, i + 1);
        entry->val = (int)i;
    }
    menunode_t *eject = jgrf_menu_node_add_child(node);
    snprintf(eject->desc, DESCSIZE, "Eject");
    eject->val = -1;
}

// Change menu level
static void jgrf_menu_level(void) {
    menunode_t *node = menulevel;
    unsigned numlines = 0;
    ezmenu_setheader(&ezm, node->parent->desc);
    while (node) {
        snprintf(linebuf[numlines++], DESCSIZE, "%s", node->desc);
        node = node->next;
    }
    ezmenu_setlines(&ezm, linebuf, numlines);
}

// Redraw the text to be displayed on screen
void jgrf_menu_text_redraw(void) {
    textbuf[0] = '\0';
    for (int i = 0; i < ezm.h; ++i) {
        // menu line being drawn (offset from header)
        int mline = i - ezm.start;

        // active setting in relation to scroll offset
        int active = settingactive - ezm.yscroll;

        if (i == ezm.vissel)
            strjcat(textbuf, "> ", sizeof(textbuf));
        else if (i > 1 && strlen(ezm.vislines[i]))
            strjcat(textbuf, "  ", sizeof(textbuf));

        strjcat(textbuf, ezm.vislines[i], sizeof(textbuf));

        if (settinglevel && i >= ezm.start && i < ezm.end && active == mline)
            strjcat(textbuf, " *\n", sizeof(textbuf));
        else
            strjcat(textbuf, "\n", sizeof(textbuf));
    }
    jgrf_video_text(2, 1, textbuf);

    /* Ensure the menu can be redrawn if this function is called explicitly
       after redrawing has been disabled
    */
    skip_redraw = 0;
}

static void jgrf_menu_select_frontend(int item) {
    menunode_t *node = menulevel;
    // Seek to the correct node
    for (int i = 0; i < item; ++i)
        node = node->next;

    if (!settinglevel && !node->child->child)
        settinglevel = 1;

    // Check if there is a level below
    if (node->child) {
        menulevel = node->child;
        jgrf_menu_level();
        if (settinglevel)
            settingactive = settings[item].val - settings[item].min;
    }
    else { // No child, means it has a value to set
        settings[node->parent->val].val = node->val;
        settinglevel = 1;
        settingactive = item;
        jgrf_rehash_frontend();
        jgrf_log(JG_LOG_SCR, "%s: %s%s",
            node->parent->desc, node->desc,
            settings[node->parent->val].flags & JG_SETTING_RESTART ?
            " (restart required)" : ""
        );
        menupath >>= 8;
    }
}

// Emulator Settings and DIP Switches
static void jgrf_menu_select_settings(int item, jg_setting_t *arr, size_t num) {
    if (!num) {
        jgrf_log(JG_LOG_SCR, "No Settings");
        return;
    }

    menunode_t *node = menulevel;
    // Seek to the correct node
    for (int i = 0; i < item; ++i)
        node = node->next;

    if (!settinglevel && !node->child->child)
        settinglevel = 1;

    // Check if there is a level below
    if (node->child) {
        menulevel = node->child;
        jgrf_menu_level();
        if (settinglevel)
            settingactive = arr[item].val - arr[item].min;
    }
    else { // No child, means it has a value to set
        arr[node->parent->val].val = node->val;
        settinglevel = 1;
        settingactive = item;
        jgrf_rehash_core();

        if (arr[node->parent->val].flags & JG_SETTING_INPUT)
            jgrf_rehash_input();

        jgrf_log(JG_LOG_SCR, "%s: %s%s",
            node->parent->desc, node->desc,
            arr[node->parent->val].flags & JG_SETTING_RESTART ?
            " (restart required)" : ""
        );
        menupath >>= 8;
    }
}

static void jgrf_menu_select_input(int item) {
    menunode_t *node = menulevel;
    // Seek to the correct node
    for (int i = 0; i < item; ++i) {
        node = node->next;
    }

    // Check if there is a level below
    if (node->child) {
        menulevel = node->child;
        jgrf_menu_level();
    }
    else { // No child, means it can be configured
        int slot = node->val;
        if (inputlist && slot >= 0 && (size_t)slot < inputlist_count) {
            jgrf_input_config_enable(1);
            jgrf_input_config(&inputlist[slot]);
            skip_redraw = 1;
        }

        // Ensure the menu system knows it has not really gone a layer deeper
        ezm.sel = menupath & 0xff;
        menupath >>= 8;
        ezmenu_update(&ezm);
    }
}

static void jgrf_menu_select_media(int item) {
    menunode_t *node = menulevel;
    for (int i = 0; i < item; ++i)
        node = node->next;

    if (node->child) {
        settinglevel = 1;
        menulevel = node->child;
        jgrf_menu_level();
        settingactive = mediainfo->inserted
            ? (int)mediainfo->index : (int)mediainfo->count;
    }
    else {
        if (node->val < 0) { // Eject
            if (mediainfo->inserted)
                jgrf_media_mount(0);
            settingactive = (int)mediainfo->count;
        }
        else { // Select and insert
            if (mediainfo->inserted && mediainfo->index == (unsigned)node->val)
                return;
            if (mediainfo->inserted)
                jgrf_media_mount(0); // Unmount current
            jgrf_media_set((unsigned)node->val);
            jgrf_media_mount(1); // Mount new
            settingactive = item;
        }
        settinglevel = 1;
        menupath >>= 8;
    }
}

static void jgrf_menu_select_save(int item) {
    menunode_t *node = menulevel;
    // Seek to the correct node
    for (int i = 0; i < item; ++i)
        node = node->next;

    // Check if there is a level below
    if (node->child) {
        menulevel = node->child;
        jgrf_menu_level();
    }
    else {
        switch (item) {
#ifdef JGRF_STATIC
            case 0: {
                jgrf_settings_write(SETTINGS_FRONTEND|SETTINGS_EMULATOR);
                jgrf_log(JG_LOG_SCR, "Saved Settings");
                break;
            }
            case 1: {
                jgrf_settings_default(SETTINGS_FRONTEND);
                jgrf_log(JG_LOG_SCR, "Restored Frontend Defaults");
                break;
            }
            case 2: {
                jgrf_settings_default(SETTINGS_EMULATOR);
                jgrf_log(JG_LOG_SCR, "Restored Emulator Defaults");
                break;
            }
            case 3: {
                jgrf_dips_default();
                jgrf_log(JG_LOG_SCR, "Restored DIP Defaults");
                break;
            }
#else
            case 0: {
                jgrf_settings_write(SETTINGS_FRONTEND);
                jgrf_log(JG_LOG_SCR, "Saved Frontend Settings");
                break;
            }
            case 1: {
                if (numemusettings) {
                    jgrf_settings_write(SETTINGS_EMULATOR);
                    jgrf_log(JG_LOG_SCR, "Saved Emulator Settings");
                }
                else {
                    jgrf_log(JG_LOG_SCR, "No Emulator Settings");
                }
                break;
            }
            case 2: {
                jgrf_settings_write(SETTINGS_FRONTEND|SETTINGS_EMULATOR);
                jgrf_log(JG_LOG_SCR, "Saved Combined Settings");
                break;
            }
            case 3: {
                jgrf_settings_default(SETTINGS_FRONTEND);
                jgrf_log(JG_LOG_SCR, "Restored Frontend Defaults");
                break;
            }
            case 4: {
                jgrf_settings_default(SETTINGS_EMULATOR);
                jgrf_log(JG_LOG_SCR, "Restored Emulator Defaults");
                break;
            }
            case 5: {
                jgrf_dips_default();
                jgrf_log(JG_LOG_SCR, "Restored DIP Defaults");
                break;
            }
#endif
        }
        menupath >>= 8;
    }
}

// Initialize and display the menu on screen
void jgrf_menu_display(void) {
    // Grab the frontend settings
    settings = jgrf_settings_ptr();

    // Grab the emulator settings
    emusettings = jgrf_settings_emu_ptr(&numemusettings);

    // Grab the DIP switches (may be empty for most games)
    dips = jgrf_dips_ptr(&numdips);

    // Grab media info (may be NULL for non-disc/disk games)
    mediainfo = jgrf_jgapi_ptr()->jg_get_mediainfo
        ? jgrf_jgapi_ptr()->jg_get_mediainfo() : NULL;

    menuroot = jgrf_menu_node_create();
    snprintf(menuroot->desc, DESCSIZE, "%s", jgrf_gdata_ptr()->corefname);
    menunode_t *node;

    node = jgrf_menu_node_add_child(menuroot);
    node->mode = FRONTEND;
    snprintf(node->desc, DESCSIZE, "Frontend Settings");
    jgrf_menu_gen(node, settings, JGRF_SETTINGS_MAX);

    if (numemusettings) {
        node = jgrf_menu_node_add_child(menuroot);
        node->mode = EMULATOR;
        snprintf(node->desc, DESCSIZE, "Emulator Settings");
        jgrf_menu_gen(node, emusettings, numemusettings);
    }

    if (numdips) {
        node = jgrf_menu_node_add_child(menuroot);
        node->mode = DIPS;
        snprintf(node->desc, DESCSIZE, "DIP Switches");
        jgrf_menu_gen(node, dips, numdips);
    }

    node = jgrf_menu_node_add_child(menuroot);
    node->mode = INPUT;
    snprintf(node->desc, DESCSIZE, "Map Inputs");
    jgrf_menu_gen_input(node);

    if (mediainfo) {
        node = jgrf_menu_node_add_child(menuroot);
        node->mode = MEDIA;
        snprintf(node->desc, DESCSIZE, "Media");
        jgrf_menu_gen_media(node);
    }

    menunode_t *savenode = jgrf_menu_node_add_child(menuroot);
    savenode->mode = SAVESETTINGS;
    snprintf(savenode->desc, DESCSIZE, "Save/Restore Settings");

#ifdef JGRF_STATIC
    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Save Settings");
#else
    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Save Frontend Settings");

    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Save Emulator Settings");

    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Save Combined Settings");
#endif

    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Restore Frontend Defaults");

    node = jgrf_menu_node_add_child(savenode);
    snprintf(node->desc, DESCSIZE, "Restore Emulator Defaults");

    if (numdips) {
        node = jgrf_menu_node_add_child(savenode);
        snprintf(node->desc, DESCSIZE, "Restore DIP Defaults");
    }

    for (unsigned i = 0; i < NUMLINES; ++i)
        linebuf[i] = (char*)calloc(DESCSIZE, 1);

    menulevel = menuroot->child;
    menupath = 0;
    ezmenu_init(&ezm, vidinfo->w, vidinfo->h, 10, 12);
    ezmenu_setheader(&ezm, menuroot->desc);
    ezmenu_setfooter(&ezm, "");
    ezm.wraparound = 1;
    jgrf_menu_level();
    ezmenu_update(&ezm);
    jgrf_menu_text_redraw();
}

void jgrf_menu_input_handler(SDL_Event *event) {
    switch (event->key.scancode) {
        case SDL_SCANCODE_TAB: case SDL_SCANCODE_LEFT: {
            settinglevel = 0;
            if (menulevel != menuroot->child) {
                menulevel = menulevel->parent->parent->child;
                jgrf_menu_level();
                ezm.sel = menupath & 0xff;
                menupath >>= 8;
                ezmenu_update(&ezm);
                jgrf_menu_text_redraw();
                break;
            }
        }
        // fallthrough
        case SDL_SCANCODE_ESCAPE: {
            // Free the memory allocated for the menu tree
            jgrf_menu_node_free(menuroot);
            jgrf_video_text(2, 0, "");
            free(ezm.vislines);
            for (unsigned i = 0; i < NUMLINES; ++i)
                free(linebuf[i]);
            jgrf_input_menu_enable(0);
            settinglevel = 0;
            break;
        }
        case SDL_SCANCODE_UP: {
            ezmenu_userinput(&ezm, EZM_UP);
            jgrf_menu_text_redraw();
            break;
        }
        case SDL_SCANCODE_DOWN: {
            ezmenu_userinput(&ezm, EZM_DOWN);
            jgrf_menu_text_redraw();
            break;
        }
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_RIGHT: {
            menupath = (menupath << 8) | ezm.sel;
            if (menulevel == menuroot->child) {
                // Read the mode tag off the selected top-level child
                menunode_t *sel = menuroot->child;
                for (int i = 0; i < ezm.sel; ++i)
                    sel = sel->next;
                menumode = sel->mode;
            }

            switch (menumode) {
                case FRONTEND:
                    jgrf_menu_select_frontend(ezm.sel);
                    break;
                case EMULATOR:
                    jgrf_menu_select_settings(ezm.sel,
                        emusettings, numemusettings);
                    break;
                case DIPS:
                    jgrf_menu_select_settings(ezm.sel, dips, numdips);
                    break;
                case INPUT:
                    jgrf_menu_select_input(ezm.sel);
                    break;
                case MEDIA:
                    jgrf_menu_select_media(ezm.sel);
                    break;
                case SAVESETTINGS:
                    jgrf_menu_select_save(ezm.sel);
                    break;
            }

            ezmenu_update(&ezm);
            if (!skip_redraw)
                jgrf_menu_text_redraw();
            break;
        }
        default: {
            break;
        }
    }
}

void jgrf_menu_set_vinfo(jg_videoinfo_t *ptr) {
    vidinfo = ptr;
}
