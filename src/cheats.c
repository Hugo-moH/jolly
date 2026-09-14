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
#include <stdint.h>
#include <stdlib.h>

#include "parson.h"

#include "jgrf.h"
#include "cheats.h"
#include "settings.h"

static void (*jgrf_cheat_clear)(void);
static void (*jgrf_cheat_set)(const char *);

static char *chtfile = NULL;
static int chtactive = 0;
static int chtloaded = 0;

// Parse the cheat file and activate any cheats
void jgrf_cheats_toggle(void) {
    // If cheats are not loaded, simply return
    if (!chtloaded) {
        jgrf_log(JG_LOG_SCR, "No cheat file loaded");
        return;
    }

    if (chtactive) {
        jgrf_cheat_clear();
        chtactive = 0;
        jgrf_log(JG_LOG_SCR, "Cheats Deactivated");
        return;
    }

    jgrf_cheat_clear(); // Disable any active cheats first

    // Parse the JSON string
    JSON_Value *root_value = json_parse_string_with_comments(chtfile);

    if (root_value == NULL || json_value_get_type(root_value) != JSONObject) {
        jgrf_log(JG_LOG_WRN, "Failed to parse cheat file\n");
        return;
    }

    // Retrieve the cheats array
    JSON_Object *parent = json_value_get_object(root_value);
    JSON_Array *cheatarray = json_object_get_array(parent, "cheats");

    // Loop through all the cheats
    for (size_t i = 0; i < json_array_get_count(cheatarray); ++i) {
        JSON_Object *cheat = json_array_get_object(cheatarray, i);

        // Apply any cheats set to "enabled"
        if (json_object_get_boolean(cheat, "enabled")) {
            chtactive = 1;
            jgrf_log(JG_LOG_DBG,
                "Cheat: %s\n", json_object_get_string(cheat, "desc"));

            // Enable all codes in the array
            JSON_Array *codes = json_object_get_array(cheat, "codes");
            for (size_t j = 0; j < json_array_get_count(codes); ++j)
                jgrf_cheat_set(json_array_get_string(codes, j));
        }
    }

    if (chtactive)
        jgrf_log(JG_LOG_SCR, "Cheats Activated");

    // Free resources associated with parsing the JSON string
    json_value_free(root_value);
}

void jgrf_cheats_deinit(void) {
    if (chtfile)
        free(chtfile);
}

void jgrf_cheats_init(void (*chtclear)(void), void (*chtset)(const char *)) {
    // Set the function pointers
    jgrf_cheat_clear = chtclear;
    jgrf_cheat_set = chtset;

    // Grab the pointer to global data
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Set the cheat file path
    char chtfilepath[264];
    snprintf(chtfilepath, sizeof(chtfilepath), "%s/%s.json",
        gdata->cheatpath, gdata->gamename);

    // Attempt to open the file
    FILE *file = fopen(chtfilepath, "rb");

    if (!file) {
        jgrf_log(JG_LOG_DBG, "No cheat file: %s\n", chtfilepath);
        return;
    }

    fseek(file, 0, SEEK_END);
    size_t chtfilesize = ftell(file) * sizeof(char);
    rewind(file);

    chtfile = (char*)calloc(chtfilesize, sizeof(char));

    if (!chtfile || !fread((void*)chtfile, chtfilesize, 1, file)) {
        jgrf_log(JG_LOG_WRN, "Failed to read file: %s\n", chtfilepath);
        fclose(file);
        return;
    }

    jgrf_log(JG_LOG_DBG, "Cheat file loaded: %s\n", chtfilepath);

    // Cheats have been loaded
    chtloaded = 1;

    // Activate any cheats
    if (jgrf_settings_ptr()[MISC_CHEATAUTO].val)
        jgrf_cheats_toggle();
}
