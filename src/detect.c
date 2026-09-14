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

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "musl_memmem.h"

#include "jgrf.h"
#include "detect.h"

#define NUMCORES 3 // Up to 3 possible core choices

typedef struct _corelist_t {
    const char *sys; // System
    const char *corename[NUMCORES];
} corelist_t;

static corelist_t corelist[] = {
    {"32x",         {"picodrive", "", ""}},
    {"7800",        {"prosystem", "", ""}},
    {"coleco",      {"jollycv", "", ""}},
    {"gb",          {"gambatte", "sameboy", ""}},
    {"gba",         {"mgba", "", ""}},
    {"gg",          {"cega", "", ""}},
    {"md",          {"genplus", "mednafen", "picodrive"}},
    {"myvision",    {"jollycv", "", ""}},
    {"lynx",        {"mednafen", "", ""}},
    {"n64",         {"cen64", "", ""}},
    {"nds",         {"melonds", "", ""}},
    {"neogeo",      {"geolith", "", ""}},
    {"neogeocd",    {"geolith", "", ""}},
    {"nes",         {"nestopia", "", ""}},
    {"ngp",         {"mednafen", "", ""}},
    {"pce",         {"mednafen", "", ""}},
    {"pcfx",        {"mednafen", "", ""}},
    {"psx",         {"mednafen", "", ""}},
    {"sg",          {"cega", "", ""}},
    {"sms",         {"cega", "", ""}},
    {"snes",        {"bsnes", "mednafen", ""}},
    {"ss",          {"mednafen", "", ""}},
    {"vb",          {"mednafen", "", ""}},
    {"vectrex",     {"vecx", "", ""}},
    {"wswan",       {"mednafen", "", ""}}
};

typedef struct _extmap_t {
    const char *ext;
    const char *sys;
} extmap_t;

static extmap_t extmap[] = {
    {"32x", "32x"},
    {"a78", "7800"},
    {"bs",  "snes"},
    {"chd", "neogeocd"}, // Only Geolith supports .chd
    {"col", "coleco"},
    {"dsi", "nds"},
    {"fds", "nes"},
    {"gb",  "gb"},
    {"gba", "gba"},
    {"gbc", "gb"},
    {"gg",  "gg"},
    {"lnx", "lynx"},
    {"myv", "myvision"},
    {"md",  "md"},
    {"n64", "n64"},
    {"ndd", "n64"},
    {"nds", "nds"},
    {"neo", "neogeo"},
    {"nes", "nes"},
    {"ngc", "ngp"},
    {"ngp", "ngp"},
    {"nsf", "nes"},
    {"pce", "pce"},
    {"rom", "coleco"},
    {"sfc", "snes"},
    {"sg",  "sg"},
    {"sgx", "pce"},
    {"smc", "snes"},
    {"sms", "sms"},
    {"st",  "snes"},
    {"vb",  "vb"},
    {"vec", "vectrex"},
    {"ws",  "wswan"},
    {"wsc", "wswan"},
    {"z64", "n64"},
};

static unsigned detect_attempt = 0;

// Detect game type inside .zip archives
static int jgrf_detect_zip(const char *filename) {
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    // Make sure it's actually a zip file
    if (!mz_zip_reader_init_file(&zip_archive, filename, 0))
        return 0;

    // Cycle through files in the archive until one matches a known game type
    for (int i = 0; i < (int)mz_zip_reader_get_num_files(&zip_archive); ++i) {
        mz_zip_archive_file_stat file_stat;

        if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat)) {
            mz_zip_reader_end(&zip_archive);
            jgrf_log(JG_LOG_ERR, "Failed to stat archive. Exiting...\n");
        }

        // Set the game name based on the file inside the zip
        if (jgrf_detect_sys(file_stat.m_filename)) {
            mz_zip_reader_end(&zip_archive);
            return 1;
        }
    }

    mz_zip_reader_end(&zip_archive);

    return 0;
}

// Detect what console the optical disc was designed for
static int jgrf_detect_bincue(const char *filename, unsigned fcount) {
    uint8_t sectorbuf[2352];

    const char *mcdmagic[4] = { // Not McDonald's Magic
        "SEGADISCSYSTEM  ", "SEGABOOTDISC    ",
        "SEGADISC        ", "SEGADATADISC    ",
    };
    size_t mcdmagiclen = strlen(mcdmagic[0]); // 16

    const char *pcemagic = "PC Engine CD-ROM SYSTEM";
    size_t pcemagiclen = strlen(pcemagic); // 23

    const char *pcfxmagic = "PC-FX:Hu_CD-ROM ";
    size_t pcfxmagiclen = strlen(pcfxmagic); // 16

    const char *psxmagic = "  Licensed  by  ";
    size_t psxmagiclen = strlen(psxmagic); // 16

    const char *ssmagic = "SEGA SEGASATURN ";
    size_t ssmagiclen = strlen(ssmagic); // 16

    const char *ngcdmagic = "NEO-GEO";
    size_t ngcdmagiclen = strlen(ngcdmagic); // 8

    FILE *file;
    file = fopen(filename, "r");

    if (!file)
        return 0;

    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Check heuristically for each system, least complex first
    // PSX
    fseek(file, 0x24e0, SEEK_SET); // 0x24e0 is the magic offset
    if (fread(sectorbuf, 1, 16, file)) {
        if (!memcmp(sectorbuf, psxmagic, psxmagiclen)) {
            snprintf(gdata->sys, sizeof(gdata->sys), "psx");
            fclose(file);
            return 1;
        }
    }
    rewind(file);

    // Sega Saturn
    if (fread(sectorbuf, 1, 32, file)) { // Read data for both possible offsets
        if (!memcmp(sectorbuf, ssmagic, ssmagiclen) ||
            !memcmp(sectorbuf + 16, ssmagic, ssmagiclen)) {
            snprintf(gdata->sys, sizeof(gdata->sys), "ss");
            fclose(file);
            return 1;
        }
    }
    rewind(file);

    // Mega/Sega CD
    if (fread(sectorbuf, 1, 32, file)) { // Read data for both possible offsets
        for (int i = 0; i < 4; ++i) {
            if (!memcmp(sectorbuf, mcdmagic[i], mcdmagiclen) ||
                !memcmp(sectorbuf + 16, mcdmagic[i], mcdmagiclen)) {
                snprintf(gdata->sys, sizeof(gdata->sys), "md");
                fclose(file);
                return 1;
            }
        }
    }
    rewind(file);

    #if defined(__MINGW32__) || defined(__MINGW64__)
    /* On Windows, just assume PCE, since the search using mmemmem will fail.
       This means PC-FX can't be detected on Windows.
    */
    snprintf(gdata->sys, sizeof(gdata->sys), "pce");
    jgrf_log(JG_LOG_WRN, "Heuristic checks failed, assuming PCE CD\n");
    return 1;
    #else
    // Loop through the bin, reading 2048 byte chunks at a time
    // Neo Geo CD
    if (fcount == 0) { // The signature is always in the first track
        while (fread(sectorbuf, 1, 2048, file) == 2048) {
            if (mmemmem(sectorbuf, 2048, (uint8_t*)ngcdmagic, ngcdmagiclen)
                != NULL) {
                snprintf(gdata->sys, sizeof(gdata->sys), "neogeocd");
                fclose(file);
                return 1;
            }
        }
        rewind(file);
    }

    // PCE
    while (fread(sectorbuf, 1, 2048, file) == 2048) {
        if (mmemmem(sectorbuf, 2048, (uint8_t*)pcemagic, pcemagiclen)
            != NULL) {
            snprintf(gdata->sys, sizeof(gdata->sys), "pce");
            fclose(file);
            return 1;
        }
    }
    rewind(file);

    // PC-FX
    while (fread(sectorbuf, 1, 2048, file) == 2048) {
        if (mmemmem(sectorbuf, 2048, (uint8_t*)pcfxmagic, pcfxmagiclen)
            != NULL) {
            snprintf(gdata->sys, sizeof(gdata->sys), "pcfx");
            fclose(file);
            return 1;
        }
    }
    #endif

    fclose(file);

    return 0;
}

// Read a cuesheet to know how to access tracks for further heuristic checks
static int jgrf_detect_cue(const char *filename) {
    FILE *file;
    file = fopen(filename, "r");

    if (!file) return 0;

    // Parse cuesheet for FILE entries to read from to determine system type
    char line[256]; // Should be big enough...
    char binpath[256];
    char binfullpath[512];
    unsigned fcount = 0; // File count
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "FILE")) {
            char *binfile = strchr(line, '"');
            if (binfile) binfile++;
            else continue;

            char *end = strrchr(binfile, '"');
            if (end) *end = '\0';
            else continue;

            // If we got this far, try to read into the binary file
            snprintf(binpath, sizeof(binpath), "%s", filename);
            end = strrchr(binpath, SEP);
            if (end) *(end + 1) = '\0';
            else *binpath = '\0';
            snprintf(binfullpath, sizeof(binfullpath),
                "%s%s", binpath, binfile);

            if (jgrf_detect_bincue(binfullpath, fcount)) {
                fclose(file);
                return 1;
            }

            ++fcount;
        }
    }

    fclose(file);

    return 0;
}

// Read an M3U playlist to check for relevant cuesheets to be read
static int jgrf_detect_m3u(const char *filename) {
    FILE *file;
    file = fopen(filename, "r");

    if (!file)
        return 0;

    char line[256];
    char cuepath[384]; // Cue paths are a bit larger than their containing M3U
    char cuefullpath[640];

    while (fgets(line, sizeof(line), file)) {
        // Read .cue entries from the M3U, assuming relative paths
        snprintf(cuepath, sizeof(cuepath), "%s", filename);
        line[strlen(line) - 1] = '\0'; // Remove the newline character

        // Build the absolute path to the .cue file
        char *end = strrchr(cuepath, SEP);
        if (end) *(end + 1) = '\0';
        else *cuepath = '\0';
        snprintf(cuefullpath, sizeof(cuefullpath), "%s%s", cuepath, line);

        if (jgrf_detect_cue(cuefullpath)) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);

    return 0;
}

// Set the default core for a detected system - can be overridden at CLI
int jgrf_detect_core(void) {
    if (detect_attempt == NUMCORES)
        return 0;

    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    for (size_t i = 0; i < sizeof(corelist) / sizeof(corelist_t); ++i) {
        if (!strcmp(gdata->sys, corelist[i].sys)) {
            // Ensure we are not at the end of the list
            if (!strlen(corelist[i].corename[detect_attempt]))
                return 0;

            // Copy the core name, prepare for next attempt if this one fails
            snprintf(gdata->corename, sizeof(gdata->corename), "%s",
                corelist[i].corename[detect_attempt++]);
            return 1;
        }
    }

    return 0;
}

// Detect the desired system based on file extension
int jgrf_detect_sys(const char *filename) {
    const char *extptr = strrchr(filename, '.');
    if (extptr != NULL)
        extptr++;
    else
        return 0;

    // Convert extension to lower case
    char ext[4];
    snprintf(ext, sizeof(ext), "%s", extptr);
    for (size_t i = 0; i < strlen(ext); ++i)
        ext[i] = tolower(ext[i]);

    // Grab global data pointer
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Generic file extensions
    if (!strcmp(ext, "bin"))
        return 0;
    else if (!strcmp(ext, "cue"))
        return jgrf_detect_cue(filename);
    else if (!strcmp(ext, "m3u"))
        return jgrf_detect_m3u(filename);
    else if (!strcmp(ext, "zip"))
        return jgrf_detect_zip(filename);

    // Do not load a .chd into memory
    if (!strcmp(ext, "chd"))
        gdata->pathonly = 1;

    // Non-generic file extensions
    for (size_t i = 0; i < sizeof(extmap) / sizeof(extmap_t); ++i) {
        if (!strcmp(ext, extmap[i].ext)) {
            snprintf(gdata->sys, sizeof(gdata->sys), "%s", extmap[i].sys);
            return 1;
        }
    }

    return 0;
}
