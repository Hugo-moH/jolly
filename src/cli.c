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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "musl_getopt.h"

#include "jgrf.h"
#include "cli.h"
#include "paths.h"
#include "settings.h"

// Global Data pointer
jgrf_gdata_t *gdata = NULL;

// Variables for command line options
static const char *os_def = "a:b:c:e:fg:hlo:r:s:vwx:";
static const struct option lopts[] = {
    { "video", required_argument, 0, 'a' },
    { "benchmark", required_argument, 0, 'b' },
    { "core", required_argument, 0, 'c' },
    { "sys", required_argument, 0, 'e' },
    { "fullscreen", no_argument, 0, 'f' },
    { "cheatauto", required_argument, 0, 'g' },
    { "help", no_argument, 0, 'h' },
    { "licenses", no_argument, 0, 'l' },
    { "wave", required_argument, 0, 'o' },
    { "rsqual", required_argument, 0, 'r' },
    { "shader", required_argument, 0, 's' },
    { "verbose", no_argument, 0, 'v' },
    { "window", no_argument, 0, 'w' },
    { "scale", required_argument, 0, 'x' },
    { 0, 0, 0, 0 }
};

static const char *corename = NULL;
static const char *sys = NULL;
static const char *wavfile = NULL;

static int video = -1;
static int fullscreen = 0;
static int cheatauto = -1;
static int rsqual = -1;
static int scale = -1;
static int shader = -1;
static int licenses = 0;
static int verbose = 0;
static int windowed = 0;

int waveout = 0;

// Check if a string is a number
static inline int check_str_num(const char *optname, const char *arg) {
    char *endptr;
    long num = strtol(arg, &endptr, 10);
    (void)num;
    if (endptr == arg || *endptr != '\0' || errno == ERANGE) { // Not a number
        jgrf_log(JG_LOG_WRN, "Argument for '%s' must be a number.\n", optname);
        return 0;
    }
    return 1;
}

// Return the core name specified at the command line
const char *jgrf_cli_core(void) {
    return corename;
}

// Return the emulated system name specified at the command line
const char *jgrf_cli_sys(void) {
    return sys;
}

// Return the wave file name specified at the command line
const char *jgrf_cli_wave(void) {
    return wavfile;
}

// Override settings with command line arguments
void jgrf_cli_override(void) {
    jg_setting_t *settings = jgrf_settings_ptr();

    if (verbose) {
        settings[MISC_CORELOG].val = 0;
        settings[MISC_FRONTENDLOG].val = 0;
    }

    if (video >= 0 && video <= settings[VIDEO_API].max)
        settings[VIDEO_API].val = video;
    else if (video != -1)
        jgrf_log(JG_LOG_WRN, "Setting 'video' outside of range: %d (%d-%d)\n",
            video, settings[VIDEO_API].min, settings[VIDEO_API].max);

    if (fullscreen)
        settings[VIDEO_FULLSCREEN].val = 1;

    if (cheatauto >= 0 && cheatauto <= settings[MISC_CHEATAUTO].max)
        settings[MISC_CHEATAUTO].val = cheatauto;
    else if (cheatauto != -1)
        jgrf_log(JG_LOG_WRN, "Setting 'cheatauto' outside of range: %d (%d-%d)"
            "\n", cheatauto,
            settings[MISC_CHEATAUTO].min, settings[MISC_CHEATAUTO].max);

    if (windowed)
        settings[VIDEO_FULLSCREEN].val = 0;

    if (rsqual >= 0 && rsqual <= settings[AUDIO_RSQUAL].max)
        settings[AUDIO_RSQUAL].val = rsqual;
    else if (rsqual != -1)
        jgrf_log(JG_LOG_WRN, "Setting 'rsqual' outside of range: %d (%d-%d)\n",
            rsqual, settings[AUDIO_RSQUAL].min, settings[AUDIO_RSQUAL].max);

    if (shader >= 0 && shader <= settings[VIDEO_SHADER].max)
        settings[VIDEO_SHADER].val = shader;
    else if (shader != -1)
        jgrf_log(JG_LOG_WRN, "Setting 'shader' outside of range: %d (%d-%d)\n",
            shader, settings[VIDEO_SHADER].min, settings[VIDEO_SHADER].max);

    if (scale >= 0 && scale <= settings[VIDEO_SCALE].max)
        settings[VIDEO_SCALE].val = scale;
    else if (scale != -1)
        jgrf_log(JG_LOG_WRN, "Setting 'scale' outside of range: %d (%d-%d)\n",
            scale, settings[VIDEO_SCALE].min, settings[VIDEO_SCALE].max);
}

// Command line parsing
void jgrf_cli_parse(int argc, char *argv[]) {
    int c;
    int loptind;

    while ((c = getopt_long(argc, argv, os_def, lopts, &loptind)) != -1) {
        switch (c) {
            case 'a': { // Video API
                if (check_str_num("video", optarg))
                    video = atoi(optarg);
                break;
            }
            case 'b': { // Benchmark
                long long bmframes = atoll(optarg);
                if (bmframes < 1)
                    jgrf_log(JG_LOG_WRN, "Argument for --bmark must be a "
                        "number greater than 0\n");
                else
                    jgrf_benchmark(bmframes);
                break;
            }
#ifndef JGRF_STATIC
            case 'c': { // Core selection
                corename = optarg;
                break;
            }
#endif
            case 'e': { // System to emulate
                sys = optarg;
                break;
            }
            case 'h': { // Show usage
                jgrf_cli_usage(argv[0]);
                jgrf_quit(EXIT_SUCCESS);
                break;
            }
            case 'l': { // Show licenses
                licenses = 1;
                break;
            }
            case 'f': { // Start in fullscreen mode
                fullscreen = 1;
                break;
            }
            case 'g': { // Auto-activate cheats if present
                if (check_str_num("cheatauto", optarg))
                    cheatauto = atoi(optarg);
                break;
            }
            case 'o': { // Wave File Output
                wavfile = optarg;
                struct stat fbuf;
                if (stat(wavfile, &fbuf) == 0) {
                    jgrf_log(JG_LOG_WRN,
                        "WAV Writer: Refusing to overwrite %s!\n", wavfile);
                }
                else {
                    jgrf_log(JG_LOG_WRN,
                        "WAV Writer: Writing WAV to %s\n", wavfile);
                    waveout = 1;
                }
                break;
            }
            case 'r': { // Resampler Quality
                if (check_str_num("rsqual", optarg))
                    rsqual = atoi(optarg);
                break;
            }
            case 's': { // Shader
                if (check_str_num("shader", optarg))
                    shader = atoi(optarg);
                break;
            }
            case 'v': { // Enable verbose log output for core and frontend
                verbose = 1;
                break;
            }
            case 'w': { // Start in windowed mode
                windowed = 1;
                break;
            }
            case 'x': { // Scale
                if (check_str_num("scale", optarg))
                    scale = atoi(optarg);
                break;
            }
            case '?': {
                fprintf(stderr, "Try '%s --help' for more information.\n",
                    argv[0]);
                jgrf_quit(EXIT_FAILURE);
                break;
            }
            default: {
                break;
            }
        }
    }

    // Handle --licenses after all options are parsed so -c is available
    if (licenses) {
        jgrf_cli_licenses();
        jgrf_quit(EXIT_SUCCESS);
    }

    // Last non-option is the ROM filename
    gdata = jgrf_gdata_ptr();
    gdata->filename = argv[argc - 1];

    // Count the number of auxiliary files
    gdata->numauxfiles = (unsigned)argc - optind - 1;
    if (gdata->numauxfiles > JGRF_AUXFILE_MAX)
        gdata->numauxfiles = JGRF_AUXFILE_MAX;

    for (int i = 0; i < gdata->numauxfiles; ++i)
        jgrf_auxfile_load(argv[optind + i], i);
}

void jgrf_cli_licenses(void) {
    gdata = jgrf_gdata_ptr();

    fprintf(stdout,
        "The Jolly Good Reference Frontend\n"
        "Copyright (c) 2020-2026 Rupert Carmichael\n"
        "Licensed under the BSD 3-Clause \"New\" or \"Revised\" License.\n"
        "Refer to README for dependency licenses.\n\n");

#ifdef JGRF_STATIC
    jgrf_paths_set_corelib();
    jgrf_paths_set_coreassets();
#else
    if (!corename) {
        fprintf(stdout, "Specify a core with -c to view its licenses.\n");
        return;
    }
    snprintf(gdata->corename, sizeof(gdata->corename), "%s", corename);
    if (!jgrf_paths_env_coreassets())
        jgrf_paths_set_coreassets();
#endif

    char lpath[448];
    snprintf(lpath, sizeof(lpath), "%s%cLICENSES", gdata->coreassets, SEP);

    FILE *lfile = fopen(lpath, "r");

#ifdef JGRF_STATIC
    // Fall back to the binary's directory for static builds
    if (!lfile) {
        const char *fallback = gdata->binpath[0] ? gdata->binpath : ".";
        snprintf(lpath, sizeof(lpath), "%s%cLICENSES", fallback, SEP);
        lfile = fopen(lpath, "r");
    }
#endif

    if (!lfile) {
        fprintf(stdout, "License file not found\n");
        return;
    }

    char buf[4096];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), lfile)) > 0)
        fwrite(buf, 1, nread, stdout);

    fclose(lfile);
}

void jgrf_cli_usage(char *binname) {
#ifdef JGRF_STATIC
    jg_coreinfo_t *coreinfo = jg_get_coreinfo("");
    fprintf(stdout, "%s %s\n", coreinfo->fname, coreinfo->version);
#else
    fprintf(stdout, "The Jolly Good Reference Frontend %s\n", VERSION);
#endif
    fprintf(stdout, "usage: %s [options] [auxiliary files] game\n",
        strrchr(binname, SEP) ? strrchr(binname, SEP) + 1 : binname);
    fprintf(stdout, "  options:\n");
    fprintf(stdout, "    -a, --video <value>     "
        "Specify which Video API to use\n");
    fprintf(stdout, "                              0 = OpenGL Core Profile\n");
    fprintf(stdout, "                              1 = OpenGL ES\n");
    fprintf(stdout, "                              2 = OpenGL Compatibility "
        "Profile\n");
#ifdef JGRF_VULKAN
    fprintf(stdout, "                              3 = Vulkan\n");
#endif
    fprintf(stdout, "    -b, --bmark <frames>    "
        "Run N frames in Benchmark mode\n");
#ifndef JGRF_STATIC
    fprintf(stdout, "    -c, --core <corename>   "
        "Specify which core to use\n");
#endif
    fprintf(stdout, "    -e, --sys <sysname>     "
        "Specify which system to emulate\n");
    fprintf(stdout, "    -f, --fullscreen        "
        "Start in Fullscreen mode\n");
    fprintf(stdout, "    -g, --cheatauto <value> "
        "Auto-activate cheats if present\n");
    fprintf(stdout, "                              0 = Disabled\n");
    fprintf(stdout, "                              1 = Enabled\n");
    fprintf(stdout, "    -h, --help              "
        "Show Usage\n");
    fprintf(stdout, "    -l, --licenses          "
#ifdef JGRF_STATIC
        "Show Licenses\n");
#else
        "Show Licenses for a core (requires -c)\n");
#endif
    fprintf(stdout, "    -o, --wave <filename>   "
        "Specify WAV output file\n");
    fprintf(stdout, "    -r, --rsqual <value>    "
        "Resampler Quality (0 to 10)\n");
    fprintf(stdout, "    -s, --shader <value>    "
        "Select a Post-processing shader\n");
    fprintf(stdout, "                              0 = Nearest Neighbour (None)"
        "\n");
    fprintf(stdout, "                              1 = Linear\n");
    fprintf(stdout, "                              2 = Sharp Bilinear\n");
    fprintf(stdout, "                              3 = Anti-Aliased Nearest "
        "Neighbour\n");
    fprintf(stdout, "                              4 = CRT-Yee64\n");
    fprintf(stdout, "                              5 = CRTea\n");
    fprintf(stdout, "                              6 = LCD\n");
    fprintf(stdout, "    -v, --verbose           "
        "Enable verbose log output for core and frontend\n");
    fprintf(stdout, "    -w, --window            "
        "Start in Windowed mode\n");
    fprintf(stdout, "    -x, --scale <value>     "
        "Video Scale Factor (1 to 8)\n\n");
}
