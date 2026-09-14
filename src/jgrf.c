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
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>

#include <SDL3/SDL.h>

#include "md5.h"
#include "miniz.h"

#include "jgrf.h"
#include "audio.h"
#include "cheats.h"
#include "cli.h"
#include "detect.h"
#include "dips.h"
#include "input.h"
#include "menu.h"
#include "osd.h"
#include "paths.h"
#include "settings.h"
#include "video.h"

// Jolly Good API calls
static jgapi_t jgapi;

// Keep track of which internal systems have been loaded successfully
static struct _loaded {
    int core;
    int game;
    int audio;
    int video;
    int input;
    int settings;
} loaded = { 0, 0, 0, 0, 0, 0 };

// Pointer to core info struct
static jg_coreinfo_t *coreinfo = NULL;

// Frontend knows the game and path info and passes this to the core
static jg_fileinfo_t gameinfo;
static jg_fileinfo_t auxinfo[JGRF_AUXFILE_MAX];
static jg_pathinfo_t pathinfo;

// Global data struct for miscellaneous information
static jgrf_gdata_t gdata;
jgrf_gdata_t *jgrf_gdata_ptr(void) {
    return &gdata;
}

jgapi_t *jgrf_jgapi_ptr(void) {
    return &jgapi;
}

// Program should keep running (1) or, shut down (0)
static int running = 1;

// Benchmark mode
int bmark = 0;

// Pause
int pause = 0;

// Number of extra frames to run for fast-forwarding purposes
int fforward = 0;

// Frame timing
int corefps = 60;
int screenfps = 60;
size_t framecount = 0;
size_t bmarkframes = 0;

// Discern the name of the game, with and without file extension
static void jgrf_gamename(const char *filename) {
    // Set game's name based on the path
    snprintf(gdata.gamename, sizeof(gdata.gamename),
        "%s", basename((char*)filename));
    snprintf(gdata.gamefname, sizeof(gdata.gamefname),
        "%s", basename((char*)filename));

    // Strip the file extension off
    for (int i = strlen(gdata.gamename) - 1; i > 0; --i) {
        if (gdata.gamename[i] == '.') {
            gdata.gamename[i] = '\0';
            break;
        }
    }
    gameinfo.fname = gdata.gamefname;
    gameinfo.name = gdata.gamename;
}

// Handle log output from the frontend
void jgrf_log(int level, const char *fmt, ...) {
    va_list va;
    char buffer[512];
    static const char *lchr = "diwe";
    static const char *lcol[4] = {
        "\033[0;35m", "\033[0m", "\033[0;33m", "\033[1;31m"
    };

    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    FILE *fout = level == 1 ? stdout : stderr;

    jg_setting_t *settings = jgrf_settings_ptr();
    if (level == JG_LOG_SCR) {
        jgrf_video_text(0, corefps, buffer);
    }
    else if (level >= settings[MISC_FRONTENDLOG].val) {
        fprintf(fout, "%s%c: %s\033[0m", lcol[level], lchr[level], buffer);
        fflush(fout);
    }

    if (level == JG_LOG_ERR)
        jgrf_quit(EXIT_FAILURE);
}

// Handle log output from the core
static void jgrf_core_log(int level, const char *fmt, ...) {
    va_list va;
    char buffer[512];
    static const char *lchr = "DIWE";
    static const char *lcol[4] = {
        "\033[0;35m", "\033[0;36m", "\033[7;33m", "\033[1;7;31m"
    };

    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    FILE *fout = level == 1 ? stdout : stderr;

    jg_setting_t *settings = jgrf_settings_ptr();
    if (level == JG_LOG_SCR) {
        jgrf_video_text(1, corefps, buffer);
    }
    else if (level >= settings[MISC_CORELOG].val) {
        fprintf(fout, "%s%c: %s\033[0m", lcol[level], lchr[level], buffer);
        fflush(fout);
    }

    if (level == JG_LOG_ERR)
        jgrf_quit(EXIT_FAILURE);
}

// Pass callbacks into the core
static void jgrf_callbacks_set(void) {
    jgapi.jg_set_cb_log(&jgrf_core_log);
    jgapi.jg_set_cb_audio(&jgrf_audio_cb_core, NULL);
    jgapi.jg_set_cb_frametime(&jgrf_frametime, NULL);
    jgapi.jg_set_cb_rumble(&jgrf_input_rumble, NULL);
}

// Load and set up the core
static void jgrf_core_load(const char *corepath) {
    memset(&jgapi, 0, sizeof(jgapi));
#ifdef JGRF_STATIC
    (void)corepath;
    jgapi.jg_api_version = &jg_api_version;
    jgapi.jg_init = &jg_init;
    jgapi.jg_deinit = &jg_deinit;
    jgapi.jg_reset = &jg_reset;
    jgapi.jg_exec_frame = &jg_exec_frame;
    jgapi.jg_game_load = &jg_game_load;
    jgapi.jg_game_unload = &jg_game_unload;
    jgapi.jg_state_load = &jg_state_load;
    jgapi.jg_state_load_raw = &jg_state_load_raw;
    jgapi.jg_state_save = &jg_state_save;
    jgapi.jg_state_save_raw = &jg_state_save_raw;
    jgapi.jg_state_size = &jg_state_size;
    jgapi.jg_media_set = &jg_media_set;
    jgapi.jg_media_mount = &jg_media_mount;
    jgapi.jg_cheat_clear = &jg_cheat_clear;
    jgapi.jg_cheat_set = &jg_cheat_set;
    jgapi.jg_rehash = &jg_rehash;
    jgapi.jg_data_push = &jg_data_push;

    jgapi.jg_get_coreinfo = &jg_get_coreinfo;
    jgapi.jg_get_systemlist = &jg_get_systemlist;
    jgapi.jg_get_bioslist = &jg_get_bioslist;
    jgapi.jg_get_audioinfo = &jg_get_audioinfo;
    jgapi.jg_get_videoinfo = &jg_get_videoinfo;
    jgapi.jg_get_inputinfo = &jg_get_inputinfo;
    jgapi.jg_get_inputlist = &jg_get_inputlist;
    jgapi.jg_get_settings = &jg_get_settings;
    jgapi.jg_get_mediainfo = &jg_get_mediainfo;

    jgapi.jg_setup_video = &jg_setup_video;
    jgapi.jg_setup_audio = &jg_setup_audio;
    jgapi.jg_set_inputstate = &jg_set_inputstate;
    jgapi.jg_set_gameinfo = &jg_set_gameinfo;
    jgapi.jg_set_auxinfo = &jg_set_auxinfo;
    jgapi.jg_set_paths = &jg_set_paths;

    jgapi.jg_set_cb_log = &jg_set_cb_log;
    jgapi.jg_set_cb_audio = &jg_set_cb_audio;
    jgapi.jg_set_cb_frametime = &jg_set_cb_frametime;
    jgapi.jg_set_cb_rumble = &jg_set_cb_rumble;
#else
    jgapi.handle = SDL_LoadObject(corepath);

    if (!jgapi.handle)
        jgrf_log(JG_LOG_ERR, "%s\n", SDL_GetError());

    SDL_GetError();

#define LOAD_SYM(fn) do { \
    SDL_FunctionPointer fptr = SDL_LoadFunction(jgapi.handle, #fn); \
    memcpy(&jgapi.fn, &fptr, sizeof(fptr)); \
} while (0)
    LOAD_SYM(jg_api_version);
    LOAD_SYM(jg_init);
    LOAD_SYM(jg_deinit);
    LOAD_SYM(jg_reset);
    LOAD_SYM(jg_exec_frame);
    LOAD_SYM(jg_game_load);
    LOAD_SYM(jg_game_unload);
    LOAD_SYM(jg_state_load);
    LOAD_SYM(jg_state_load_raw);
    LOAD_SYM(jg_state_save);
    LOAD_SYM(jg_state_save_raw);
    LOAD_SYM(jg_state_size);
    LOAD_SYM(jg_media_set);
    LOAD_SYM(jg_media_mount);
    LOAD_SYM(jg_cheat_clear);
    LOAD_SYM(jg_cheat_set);
    LOAD_SYM(jg_rehash);
    LOAD_SYM(jg_data_push);
    LOAD_SYM(jg_get_coreinfo);
    LOAD_SYM(jg_get_systemlist);
    LOAD_SYM(jg_get_bioslist);
    LOAD_SYM(jg_get_audioinfo);
    LOAD_SYM(jg_get_videoinfo);
    LOAD_SYM(jg_get_inputinfo);
    LOAD_SYM(jg_get_inputlist);
    LOAD_SYM(jg_get_settings);
    LOAD_SYM(jg_get_dips);
    LOAD_SYM(jg_get_mediainfo);
    LOAD_SYM(jg_setup_video);
    LOAD_SYM(jg_setup_audio);
    LOAD_SYM(jg_set_inputstate);
    LOAD_SYM(jg_set_gameinfo);
    LOAD_SYM(jg_set_auxinfo);
    LOAD_SYM(jg_set_paths);
    LOAD_SYM(jg_set_cb_log);
    LOAD_SYM(jg_set_cb_audio);
    LOAD_SYM(jg_set_cb_frametime);
    LOAD_SYM(jg_set_cb_rumble);
#undef LOAD_SYM

#endif

    if (jgapi.jg_api_version == NULL || jgapi.jg_api_version() < 10100)
        jgrf_log(JG_LOG_ERR, "Core built against unsupported API version\n");

    // Set up values in global data struct
    coreinfo = jgapi.jg_get_coreinfo(gdata.sys);

    // Read emulator settings
    jgrf_settings_emu(jgapi.jg_get_settings);

    // Match gdata.sys with coreinfo->sys in case of user overriding core at CLI
    snprintf(gdata.sys, sizeof(gdata.sys), "%s", coreinfo->sys);

    // Populate frontend's global data with information from the core
    snprintf(gdata.corefname, sizeof(gdata.corefname),
        "%s", coreinfo->fname);
    snprintf(gdata.coreversion, sizeof(gdata.coreversion),
        "%s", coreinfo->version);
#ifdef JGRF_STATIC
    snprintf(gdata.userassets, sizeof(gdata.userassets),
        "%sassets", gdata.datapath);
    snprintf(gdata.biospath, sizeof(gdata.biospath),
        "%sbios", gdata.datapath);
    snprintf(gdata.cheatpath, sizeof(gdata.cheatpath),
        "%scheats", gdata.datapath);
    snprintf(gdata.savepath, sizeof(gdata.savepath),
        "%ssave", gdata.datapath);
    snprintf(gdata.statepath, sizeof(gdata.statepath),
        "%sstate", gdata.datapath);
    gdata.numinputs = coreinfo->numinputs;
#else
    snprintf(gdata.userassets, sizeof(gdata.userassets),
        "%sassets%c%s", gdata.datapath, SEP, coreinfo->name);
    snprintf(gdata.biospath, sizeof(gdata.biospath),
        "%sbios", gdata.datapath);
    snprintf(gdata.cheatpath, sizeof(gdata.cheatpath),
        "%scheats%c%s", gdata.datapath, SEP, coreinfo->name);
    snprintf(gdata.savepath, sizeof(gdata.savepath),
        "%ssave%c%s", gdata.datapath, SEP, coreinfo->name);
    snprintf(gdata.statepath, sizeof(gdata.statepath),
        "%sstate%c%s", gdata.datapath, SEP, coreinfo->name);
    gdata.numinputs = coreinfo->numinputs;
#endif

    // Copy path values into the pathinfo struct, then feed them to the core
    pathinfo.base = gdata.datapath;
    pathinfo.core = gdata.coreassets;
    pathinfo.user = gdata.userassets;
    pathinfo.bios = gdata.biospath;
    pathinfo.save = gdata.savepath;
    jgapi.jg_set_paths(pathinfo);

    // Pass function pointers for callbacks from core to frontend
    jgrf_callbacks_set();

    // Set up the videoinfo/audioinfo pointers in the frontend
    jgrf_menu_set_vinfo(jgapi.jg_get_videoinfo());
    jgrf_video_set_info(jgapi.jg_get_videoinfo());
    jgrf_audio_set_info(jgapi.jg_get_audioinfo());

    // Hand pointers to input states to the core
    jgrf_input_set_states(jgapi.jg_set_inputstate);

    // Initialize the emulator core
    loaded.core = jgapi.jg_init();
    if (!loaded.core)
        jgrf_log(JG_LOG_ERR, "Failed to initialize core. Exiting...\n");
}

// Unload the core
static void jgrf_core_unload(void) {
    jgapi.jg_deinit();

    if (jgapi.handle)
        SDL_UnloadObject(jgapi.handle);
}

// Generate the CRC32 checksum of the game data
static void jgrf_hash_crc32(void) {
    gameinfo.crc = mz_crc32(gdata.crc, gameinfo.data, gameinfo.size);
}

// Convert a nybble's hexadecimal representation to a lower case ASCII char
static inline char jgrf_nyb_hexchar(unsigned nyb) {
    nyb &= 0xf; // Lower nybble only
    if (nyb >= 10)
        nyb += 'a' - 10;
    else
        nyb += '0';
    return (char)nyb;
}

// Generate the MD5 checksum of the game data
static void jgrf_hash_md5(void) {
    MD5_CTX c;
    size_t md5len = gameinfo.size;
    uint8_t *dataptr = gameinfo.data;
    uint8_t digest[16];
    MD5_Init(&c);
    /*while (md5len > 0) { // Use for large file sizes
        md5len > 512 ? MD5_Update(&c, dataptr, 512) :
        MD5_Update(&c, dataptr, md5len);
        md5len -= 512;
        dataptr += 512;
    }*/
    MD5_Update(&c, dataptr, md5len);
    MD5_Final(digest, &c);

    // Convert the digest to a string without dodgy calls to snprintf
    for (int i = 0; i < 16; ++i) {
        gdata.md5[i * 2] = jgrf_nyb_hexchar(digest[i] >> 4);
        gdata.md5[(i * 2) + 1] = jgrf_nyb_hexchar(digest[i]);
    }
    gdata.md5[32] = '\0';

    gameinfo.md5 = gdata.md5;
}

// Compare a file's MD5 against an expected value
static int jgrf_md5cmp(const char *path, const char *expected) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);

    uint8_t *data = (fsize > 0) ? malloc((size_t)fsize) : NULL;
    if (!data || !fread(data, (size_t)fsize, 1, file)) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);

    MD5_CTX c;
    uint8_t digest[16];
    char md5str[33];
    MD5_Init(&c);
    MD5_Update(&c, data, (size_t)fsize);
    MD5_Final(digest, &c);
    free(data);

    for (int i = 0; i < 16; ++i) {
        md5str[i * 2] = jgrf_nyb_hexchar(digest[i] >> 4);
        md5str[(i * 2) + 1] = jgrf_nyb_hexchar(digest[i]);
    }
    md5str[32] = '\0';

    return memcmp(md5str, expected, 33) == 0;
}

// Check BIOS/firmware files requested by the core
static void jgrf_bios_check(void) {
    size_t num = 0;
    jg_biosinfo_t *bl = jgapi.jg_get_bioslist(&num);
    if (!bl || !num)
        return;

    for (size_t i = 0; i < num; ++i) {
        char path[384];
        snprintf(path, sizeof(path), "%s%c%s",
            gdata.biospath, SEP, bl[i].fname);

        int result = jgrf_md5cmp(path, bl[i].md5);

        jgrf_log(JG_LOG_DBG,
            "BIOS: %s - %s %s\n", bl[i].fname, bl[i].desc,
            result < 0 ? "Missing" : result ? "OK" : "Mismatch");
    }
}

// Set number of frames for Benchmark mode
void jgrf_benchmark(size_t frames) {
    bmarkframes = frames;
    bmark = 1;
}

// Toggle pause
void jgrf_pause_toggle(void) {
    pause ^= 1;
    jgrf_osd_toggle(pause);
}

// Get pause status
int jgrf_get_paused(void) {
    return pause;
}

// Load an Auxiliary File
void jgrf_auxfile_load(const char *filename, int index) {
    FILE *file = fopen(filename, "rb");

    if (!file)
        jgrf_log(JG_LOG_ERR, "Failed to open file: %s\n", filename);

    fseek(file, 0, SEEK_END);
    auxinfo[index].size = ftell(file);
    rewind(file);

    auxinfo[index].data = calloc(auxinfo[index].size, sizeof(uint8_t));
    if (!auxinfo[index].data ||
        !fread((void*)auxinfo[index].data, auxinfo[index].size, 1, file)) {
        jgrf_log(JG_LOG_ERR, "Failed to read file: %s\n", filename);
        fclose(file);
        return;
    }

    snprintf(gdata.auxfilepath[index], sizeof(gdata.auxfilepath[index]),
        "%s", filename);
    auxinfo[index].path = filename;

    // Close the file - some cores may want to load it again on their own terms
    fclose(file);

    snprintf(gdata.auxname[index], sizeof(gdata.auxname[index]),
        "%s", basename((char*)filename));

    // Strip the file extension off
    for (int j = strlen(gdata.auxname[index]) - 1; j > 0; --j) {
        if (gdata.auxname[index][j] == '.') {
            gdata.auxname[index][j] = '\0';
            break;
        }
    }

    auxinfo[index].name = gdata.auxname[index];
}

// Load the game data from a .zip archive
static int jgrf_game_load_archive(const char *filename) {
    // Don't load archived files if the emulator expects an archive
    if (gdata.hints & JG_HINT_MEDIA_ARCHIVED)
        return 0;

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    // Make sure it's actually a zip file
    if (!mz_zip_reader_init_file(&zip_archive, filename, 0))
        return 0;

    // Open the first ROM in the archive
    for (int i = 0; i < (int)mz_zip_reader_get_num_files(&zip_archive); ++i) {
        mz_zip_archive_file_stat file_stat;

        if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat)) {
            mz_zip_reader_end(&zip_archive);
            jgrf_log(JG_LOG_ERR, "Failed to stat archive. Exiting...\n");
        }

        // Extract the data into memory
        gameinfo.size = file_stat.m_uncomp_size;
        gameinfo.data = mz_zip_reader_extract_file_to_heap(&zip_archive,
            file_stat.m_filename, NULL, 0);

        // Get the CRC32 Checksum
        gameinfo.crc = gdata.crc = file_stat.m_crc32;

        // Get the MD5 Checksum
        jgrf_hash_md5();

        // Set the game name based on the file inside the zip
        jgrf_gamename(file_stat.m_filename);

        // Set the game filename to the name of the zip
        gameinfo.path = filename;
        jgapi.jg_set_gameinfo(gameinfo);

        // If the game was not loaded correctly, then fail
        if (!jgapi.jg_game_load()) {
            mz_zip_reader_end(&zip_archive);
            jgrf_log(JG_LOG_ERR,
                "Failed to load ROM from archive. Exiting...\n");
        }

        // Why don't you clean up after yourself?
        mz_zip_reader_end(&zip_archive);
        loaded.game = 1;
        return 1;
    }

    mz_zip_reader_end(&zip_archive);

    return 0;
}

// Load a game that the core will open via path (large disc images)
static void jgrf_game_load_path(const char *filename) {
    jgrf_gamename(filename);
    gameinfo.path = filename;
    gameinfo.data = NULL;
    gameinfo.size = 0;
    jgapi.jg_set_gameinfo(gameinfo);

    if (!jgapi.jg_game_load())
        jgrf_log(JG_LOG_ERR, "Failed to load game. Exiting...\n");

    loaded.game = 1;
}

static void jgrf_game_load(const char *filename) {
    // Load a game
    if (jgrf_game_load_archive(filename))
        return;

    if (gdata.pathonly) {
        jgrf_game_load_path(filename);
        return;
    }

    FILE *file = fopen(filename, "rb");

    if (!file)
        jgrf_log(JG_LOG_ERR, "Failed to open file. Exiting...\n");

    fseek(file, 0, SEEK_END);
    gameinfo.size = ftell(file);
    rewind(file);

    gameinfo.data = calloc(gameinfo.size, sizeof(uint8_t));
    if (!gameinfo.data || !fread((void*)gameinfo.data, gameinfo.size, 1, file))
        jgrf_log(JG_LOG_ERR, "Failed to read file. Exiting...\n");

    // Close the file - some cores may want to load it again on their own terms
    fclose(file);

    // Get the CRC32 Checksum
    jgrf_hash_crc32();

    // Get the MD5 Checksum
    jgrf_hash_md5();

    jgrf_gamename(filename); // Set the game name
    gameinfo.path = filename;
    jgapi.jg_set_gameinfo(gameinfo);

    // If the game could not be loaded, there is no point continuing
    if (!jgapi.jg_game_load())
        jgrf_log(JG_LOG_ERR, "Failed to load game. Exiting...\n");

    // This item is set mostly so there can be a clean shutdown
    loaded.game = 1;

    return;
}

// Push data to the core
void jgrf_data_push(uint32_t type, int port, const void *ptr, size_t size) {
    jgapi.jg_data_push(type, port, ptr, size);
}

// Send a reset signal to the emulator core
void jgrf_reset(int hard) {
    jgapi.jg_reset(hard);
}

// Select media by index
void jgrf_media_set(unsigned index) {
    jgapi.jg_media_set(index);
}

// Mount or unmount media
void jgrf_media_mount(unsigned state) {
    jgapi.jg_media_mount(state);
}

// Rehash the emulator core's settings
void jgrf_rehash_core(void) {
    jgapi.jg_rehash();
}

// Rehash the frontend's settings
void jgrf_rehash_frontend(void) {
    jgrf_video_rehash(); // A subset of video settings allow live changes
    jgrf_input_rehash();
}

// Rehash input
void jgrf_rehash_input(void) {
    // Deinitialize core input states and undefine mappings
    jgrf_input_deinit_core();
    jgrf_input_undef();

    // Query core input devices
    jgrf_input_query(jgapi.jg_get_inputinfo);
}

// Call to stop and shut down at the end of the current iteration
void jgrf_schedule_quit(void) {
    running = 0;
}

// Shut everything down, clean up, exit program
void jgrf_quit(int status) {
    if (loaded.game) {
        jgrf_dips_save();
        jgapi.jg_game_unload();
    }
    if (loaded.core) jgrf_core_unload();
    if (loaded.audio) jgrf_audio_deinit();
    if (loaded.video) jgrf_video_deinit();
    if (loaded.input) jgrf_input_deinit();
    if (loaded.settings) jgrf_settings_deinit();
    if (gameinfo.data) free(gameinfo.data);
    for (int i = 0; i < gdata.numauxfiles; ++i)
        if (auxinfo[i].data) free(auxinfo[i].data);
    jgrf_cheats_deinit();
    jgrf_dips_deinit();
    SDL_Quit();
    exit(status);
}

// Retrieve the current fast-forward speed
int jgrf_get_speed(void) {
    return fforward;
}

// Set the fast-forward speed: N = extra emulation frames per screen frame
void jgrf_set_speed(int speed) {
    fforward = speed;
}

// Load state
void jgrf_state_load(int slot) {
    char statepath[260];
    snprintf(statepath, sizeof(statepath), "%s%c%s.st%d",
        gdata.statepath, SEP, gdata.gamename, slot);

    int success = jgapi.jg_state_load(statepath);

    success ? jgrf_log(JG_LOG_INF, "State Loaded: %s\n", statepath):
        jgrf_log(JG_LOG_WRN, "State Load failed: %s\n", statepath);

    jgrf_log(JG_LOG_SCR, "State %d %s",
        slot, success ? "loaded." : "load failed.");
}

// Save state
void jgrf_state_save(int slot) {
    char statepath[260];
    snprintf(statepath, sizeof(statepath), "%s%c%s.st%d",
        gdata.statepath, SEP, gdata.gamename, slot);

    int success = jgapi.jg_state_save(statepath);

    success ? jgrf_log(JG_LOG_INF, "State Saved: %s\n", statepath):
        jgrf_log(JG_LOG_WRN, "State Save failed: %s\n", statepath);

    jgrf_log(JG_LOG_SCR, "State %d %s",
        slot, success ? "saved." : "save failed.");
}

void jgrf_set_screenfps(int fps) {
    if (!fps)
        return;

    screenfps = fps;
    jgrf_log(JG_LOG_DBG, "Screen base FPS set: %dfps\n", screenfps);
}

// Callback to inform the frontend of current core framerate
void jgrf_frametime(void *udata, double frametime) {
    (void)udata;
    jgrf_audio_timing(frametime);
    corefps = frametime + 0.5;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        jgrf_cli_usage(argv[0]);
        jgrf_quit(EXIT_SUCCESS);
    }

    // Determine the binary's path relative to the current working directory
    if (strrchr(argv[0], SEP)) {
        int lastdirpos = strrchr(argv[0], SEP) - argv[0];
        strncpy(gdata.binpath, argv[0], sizeof(gdata.binpath));
        gdata.binpath[lastdirpos] = '\0';
    }
    else {
        snprintf(gdata.binpath, sizeof(gdata.binpath), ".");
    }

    // Parse command line options
    jgrf_cli_parse(argc, argv);

    // Force DirectSound audio driver on Windows
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
    putenv("SDL_AUDIO_DRIVER=directsound");
#endif

    // Allow joystick input when the window is not focused
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // Keep window fullscreen if the window manager tries to iconify it
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        jgrf_log(JG_LOG_ERR, "Failed to initialize SDL: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    // Set up user directories
    jgrf_paths_set_userdirs();

    // Load settings
    loaded.settings = jgrf_settings_init();

    // Set up function pointers for video
    jgrf_video_setfuncs();

    // Detect the system required to play the game
    if (gdata.filename == NULL) {
        jgrf_cli_usage(argv[0]);
        jgrf_log(JG_LOG_ERR, "Invalid file specified. Exiting...\n");
    }

    if (jgrf_cli_sys())
        snprintf(gdata.sys, sizeof(gdata.sys), "%s", jgrf_cli_sys());
    else
        jgrf_detect_sys(gdata.filename);

    // Set the core library and asset paths
    if (!jgrf_paths_env_corelib())
        jgrf_paths_set_corelib();
    if (!jgrf_paths_env_coreassets())
        jgrf_paths_set_coreassets();

    // Load the core
    jgrf_core_load(gdata.corelib);

    // Override any core specific settings
    jgrf_settings_override(gdata.corename);

    // Do final overrides using command line options
    jgrf_cli_override();

    // Create any directories that are required
    jgrf_paths_mkdirs();

    // Load Auxiliary files
    for (int i = 0; i < gdata.numauxfiles; ++i)
        jgapi.jg_set_auxinfo(auxinfo[i], i);

    // Set hints from the core
    gdata.hints = coreinfo->hints;

    // Show frontend/API version information
    jgrf_log(JG_LOG_INF, "The Jolly Good Reference Frontend %s\n", VERSION);
    jgrf_log(JG_LOG_DBG, "%s\n", JG_VERSION_NAME);

    // Show core information
    jgrf_log(JG_LOG_INF, "Core: %s (%s %s)\n",
        gdata.corename, gdata.corefname, gdata.coreversion);
    jgrf_log(JG_LOG_DBG, "Core System: %s\n", gdata.sys);
    jgrf_log(JG_LOG_DBG, "Core Library Path: %s\n", gdata.corelib);
    jgrf_log(JG_LOG_DBG, "Core Asset Path: %s\n", gdata.coreassets);

    // Check BIOS/firmware file status
    jgrf_bios_check();

    // Load the game
    jgrf_game_load(gdata.filename);
    if (gameinfo.data)
        jgrf_log(JG_LOG_DBG, "CRC: %08X, MD5: %s\n",
            gameinfo.crc, gameinfo.md5);

    // Load any saved DIP switch settings
    jgrf_dips_load();

    // Update hints in case loading a game caused a change
    gdata.hints = coreinfo->hints;

    // Set up function pointers for video
    jgrf_video_setfuncs();
    jgrf_video_set_info(jgapi.jg_get_videoinfo());

    // Show supported systems
    size_t numsystems = 0;
    jg_systeminfo_t *systemlist = jgapi.jg_get_systemlist(&numsystems);
    for (size_t i = 0; i < numsystems; ++i)
        jgrf_log(JG_LOG_DBG, "System: %s (%s), Extensions: %s\n",
            systemlist[i].fname, systemlist[i].name, systemlist[i].ext);

    // Set up video in the frontend and the core
    loaded.video = jgrf_video_init();
    jgapi.jg_setup_video();

    // Create the window
    jgrf_video_create();

    // Initialize audio output
    loaded.audio = jgrf_audio_init();
    jgapi.jg_setup_audio();

    // Initialize input
    loaded.input = jgrf_input_init();

    // Query core input devices
    jgrf_input_query(jgapi.jg_get_inputinfo);

    // Reset the core
    jgapi.jg_reset(2);

    // Activate Cheats
    jgrf_cheats_init(jgapi.jg_cheat_clear, jgapi.jg_cheat_set);

    // Allow the sound to flow
    jgrf_audio_unpause();

    // Explicitly disable the screensaver
    SDL_DisableScreenSaver();

    // SDL_Event struct to be filled and passed to SDL event queue
    SDL_Event event;

    int runframes = 0;
    double collector = 0;

    while (running) {
        // Divide the core framerate by the base framerate
        runframes = (corefps / screenfps); // Screenfps is monitor refresh

        // Collect the remainder of the same division operation
        collector += ((double)corefps / screenfps) - runframes;

        // When sufficient remainder has been collected, run an extra frame
        if (collector >= 1.0) {
            ++runframes;
            collector -= 1.0;
        }

        // Don't run emulation when paused. Add frames if fast-forwarding.
        if (pause)
            runframes = 0;
        else
            runframes += fforward;

        // Run the required number of emulator iterations (frames)
        for (int i = 0; i < runframes; ++i)
            jgapi.jg_exec_frame();

        framecount += runframes;

        // Render and output the current video
        jgrf_video_render(runframes);
        jgrf_video_swapbuffers();

        if (bmark && framecount >= bmarkframes) {
            jgrf_log(JG_LOG_INF, "Benchmark completed after %ld frames\n",
                bmarkframes);
            jgrf_quit(EXIT_SUCCESS);
        }

        // Poll for events
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: {
                    running = 0;
                    break;
                }
                case SDL_EVENT_WINDOW_MOVED:
                case SDL_EVENT_WINDOW_RESIZED: {
                    jgrf_video_resize();
                    break;
                }
                default: {
                    jgrf_input_handler(&event);
                    break;
                }
            }
        }
    }

    // Clean up before exiting
    jgrf_quit(EXIT_SUCCESS);

    return 0;
}
