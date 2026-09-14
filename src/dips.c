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
#include "dips.h"

static ini_table_s *conf;

static jgrf_gdata_t *gdata;

static size_t numdips = 0;
static jg_setting_t *dips = NULL;
static int *dips_default = NULL;

// Build the path to the .dip file for the loaded game
static void jgrf_dips_path(char *out, size_t outlen) {
    snprintf(out, outlen, "%s%c%s.dip", gdata->savepath, SEP, gdata->gamename);
}

// Pull DIP descriptors from the core and load saved DIP positions
void jgrf_dips_load(void) {
    gdata = jgrf_gdata_ptr();
#ifdef JGRF_STATIC
    dips = jg_get_dips(&numdips);
#else
    dips = jgrf_jgapi_ptr()->jg_get_dips(&numdips);
#endif
    if (!numdips) {
        jgrf_log(JG_LOG_DBG, "No DIP switches\n");
        return;
    }

    // Snapshot defaults so the user can reset
    dips_default = (int*)calloc(numdips, sizeof(int));
    for (size_t i = 0; i < numdips; ++i)
        dips_default[i] = dips[i].val;

    // Load saved .dip if present
    char path[384];
    jgrf_dips_path(path, sizeof(path));

    conf = ini_table_create();
    if (!ini_table_read_from_file(conf, path)) {
        jgrf_log(JG_LOG_DBG, "DIP file not found: %s\n", path);
        ini_table_destroy(conf);
        return;
    }

    char key[32];
    for (size_t i = 0; i < numdips; ++i) {
        snprintf(key, sizeof(key), "%zu", i);
        if (ini_table_check_entry(conf, "dipswitch", key)) {
            int val;
            ini_table_get_entry_as_int(conf, "dipswitch", key, &val);
            if (val >= dips[i].min && val <= dips[i].max)
                dips[i].val = val;
            else
                jgrf_log(JG_LOG_WRN,
                    "DIP value out of range at index %zu: %d\n", i, val);
        }
    }

    ini_table_destroy(conf);
}

// Write current DIP state if any DIPs exist
void jgrf_dips_save(void) {
    if (!numdips)
        return;

    char path[384];
    jgrf_dips_path(path, sizeof(path));

    conf = ini_table_create();

    char key[32];
    char ibuf[32];
    for (size_t i = 0; i < numdips; ++i) {
        snprintf(key, sizeof(key), "%zu", i);
        snprintf(ibuf, sizeof(ibuf), "%d", dips[i].val);
        ini_table_create_entry(conf, "dipswitch", key, ibuf);
    }

    ini_table_write_to_file(conf, path);
    ini_table_destroy(conf);
}

// Reset all DIPs to core-provided defaults
void jgrf_dips_default(void) {
    if (!numdips || !dips_default)
        return;

    for (size_t i = 0; i < numdips; ++i)
        dips[i].val = dips_default[i];

    jgrf_rehash_core();
}

void jgrf_dips_deinit(void) {
    if (dips_default) {
        free(dips_default);
        dips_default = NULL;
    }
    numdips = 0;
    dips = NULL;
}

jg_setting_t* jgrf_dips_ptr(size_t *num) {
    *num = numdips;
    return dips;
}
