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

#ifndef MAIN_H
#define MAIN_H

#define VERSION "2.0.1"

#define JGRF_AUXFILE_MAX 3

#ifdef __APPLE__
    #define SOEXT "dylib"
#elif defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
    #define SOEXT "dll"
#else
    #define SOEXT "so"
#endif

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
    #define SEP '\\'
    #define PATHSEP ";"
#else
    #define SEP '/'
    #define PATHSEP ":"
#endif

#include <jg/jg.h>

typedef struct jgrf_gdata_t { // Global Data
    const char *filename;
    char binpath[128]; // Directory containing the binary
    char configpath[128]; // Base user config path
    char datapath[64]; // Base user data path
    char corename[64]; // Internally used core name
    char corefname[96]; // Core Full Name
    char coreversion[32]; // Core Version
    char gamename[128]; // Internally used game name
    char gamefname[128]; // Internally used game name with extension
    char corelib[384]; // Core library path
    char coreassets[384]; // Core asset path
    char userassets[128]; // User asset path
    char biospath[128]; // BIOS path
    char cheatpath[128]; // Cheat path
    char statepath[128]; // State path
    char savepath[128]; // Save path
    char sspath[128]; // Screenshot path
    char auxfilepath[JGRF_AUXFILE_MAX][128]; // Auxiliary file paths
    char auxname[JGRF_AUXFILE_MAX][128]; // Auxiliary file names minus path/ext
    uint32_t crc; // CRC32 Checksum
    char md5[33]; // MD5 Checksum
    char sys[24]; // Name of emulated system
    int numinputs; // Number of ports/inputs in core
    int numauxfiles; // Number of loaded support files
    uint32_t hints; // Hints from the core
    int pathonly; // Open via path in the core, do not load into memory
} jgrf_gdata_t;

jgrf_gdata_t *jgrf_gdata_ptr(void);

typedef struct jgapi_t {
    void *handle;
    // Function Pointers
    unsigned (*jg_api_version)(void);
    int (*jg_init)(void);
    void (*jg_deinit)(void);
    void (*jg_reset)(int);
    void (*jg_exec_frame)(void);
    int (*jg_game_load)(void);
    int (*jg_game_unload)(void);
    int (*jg_state_load)(const char*);
    void (*jg_state_load_raw)(const void*);
    int (*jg_state_save)(const char*);
    const void* (*jg_state_save_raw)(void);
    size_t (*jg_state_size)(void);
    void (*jg_media_set)(unsigned);
    void (*jg_media_mount)(unsigned);
    void (*jg_cheat_clear)(void);
    void (*jg_cheat_set)(const char*);
    void (*jg_rehash)(void);
    void (*jg_data_push)(uint32_t, int, const void*, size_t);
    // Callback Setup
    void (*jg_set_cb_log)(jg_cb_log_t);
    void (*jg_set_cb_audio)(jg_cb_audio_t, void*);
    void (*jg_set_cb_frametime)(jg_cb_frametime_t, void*);
    void (*jg_set_cb_rumble)(jg_cb_rumble_t, void*);
    // Retrieve "info" structs from the core
    jg_coreinfo_t* (*jg_get_coreinfo)(const char*);
    jg_systeminfo_t* (*jg_get_systemlist)(size_t*);
    jg_videoinfo_t* (*jg_get_videoinfo)(void);
    jg_audioinfo_t* (*jg_get_audioinfo)(void);
    jg_inputinfo_t* (*jg_get_inputinfo)(int);
    jg_biosinfo_t* (*jg_get_bioslist)(size_t*);
    jg_inputinfo_t* (*jg_get_inputlist)(size_t*);
    jg_setting_t* (*jg_get_settings)(size_t*);
    jg_setting_t* (*jg_get_dips)(size_t*);
    jg_mediainfo_t* (*jg_get_mediainfo)(void);
    // Core setup
    void (*jg_setup_video)(void);
    void (*jg_setup_audio)(void);
    void (*jg_set_inputstate)(jg_inputstate_t*, int);
    void (*jg_set_gameinfo)(jg_fileinfo_t);
    void (*jg_set_auxinfo)(jg_fileinfo_t, int);
    void (*jg_set_paths)(jg_pathinfo_t);
} jgapi_t;

jgapi_t *jgrf_jgapi_ptr(void);

void jgrf_log(int, const char*, ...);

void jgrf_auxfile_load(const char*, int);

void jgrf_benchmark(size_t);
void jgrf_pause_toggle(void);
int jgrf_get_paused(void);

void jgrf_data_push(uint32_t, int, const void*, size_t);

void jgrf_state_load(int);
void jgrf_state_save(int);

void jgrf_reset(int);

void jgrf_media_set(unsigned);
void jgrf_media_mount(unsigned);

void jgrf_rehash_core(void);
void jgrf_rehash_frontend(void);
void jgrf_rehash_input(void);

void jgrf_schedule_quit(void);
void jgrf_quit(int);

int jgrf_get_speed(void);
void jgrf_set_speed(int);
void jgrf_set_screenfps(int);

void jgrf_state_save(int);
void jgrf_state_load(int);

void jgrf_frametime(void*, double);

extern int bmark; // External benchmark mode variable
extern int fforward; // External fast-forward level
extern int corefps; // Emulator core's framerate rounded to nearest int
extern int screenfps; // Screen's framerate rounted to nearest int

#endif
