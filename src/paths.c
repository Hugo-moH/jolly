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

// Programmer beware! Ugly code lives below.

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "jgrf.h"

#include "cli.h"
#include "detect.h"
#include "paths.h"

#if defined(__MINGW32__) || defined(__MINGW64__)
#define MKDIRR mkdir(tmp)
#else
#define MKDIRR mkdir(tmp, S_IRWXU)
#endif

// Recursive mkdir (similar to mkdir -p)
static void mkdirr(const char *dir) {
    char tmp[256];
    char *p = NULL;

    size_t len = snprintf(tmp, sizeof(tmp), "%s", dir);

    if (tmp[len - 1] == SEP)
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; ++p) {
        if (*p == SEP) {
            *p = 0;
            MKDIRR;
            *p = SEP;
        }
    }
    MKDIRR;
}

// Create user directories
void jgrf_paths_mkdirs(void) {
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();
    mkdirr(gdata->configpath);
    mkdirr(gdata->datapath);
    mkdirr(gdata->biospath);
    mkdirr(gdata->userassets);
    mkdirr(gdata->cheatpath);
    mkdirr(gdata->savepath);
    mkdirr(gdata->statepath);
    mkdirr(gdata->sspath);
}

void jgrf_paths_set_userdirs(void) {
#if defined(JGRF_STATIC)
    const char *binname = jg_get_coreinfo("")->name;
#else
    const char *binname = "jollygood";
#endif

    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Set up user config path
#if defined(JGRF_STATIC) && (defined(_WIN32) \
    || defined(__MINGW32__) || defined(__MINGW64__))
    (void)binname;
    snprintf(gdata->configpath, sizeof(gdata->configpath), "%s%c",
        gdata->binpath, SEP);
    snprintf(gdata->datapath, sizeof(gdata->datapath), "%s%c",
        gdata->binpath, SEP);
#else
    if (getenv("XDG_CONFIG_HOME"))
        snprintf(gdata->configpath, sizeof(gdata->configpath),
            "%s%c%s%c", getenv("XDG_CONFIG_HOME"), SEP, binname, SEP);
    else
        snprintf(gdata->configpath, sizeof(gdata->configpath),
            "%s%c.config%c%s%c", getenv("HOME"), SEP, SEP, binname, SEP);

    // Set up user data path
    if (getenv("XDG_DATA_HOME"))
        snprintf(gdata->datapath, sizeof(gdata->datapath),
            "%s%c%s%c", getenv("XDG_DATA_HOME"), SEP, binname, SEP);
    else
        snprintf(gdata->datapath, sizeof(gdata->datapath),
            "%s%c.local%cshare%c%s%c",
            getenv("HOME"), SEP, SEP, SEP, binname, SEP);
#endif

    // Set up screenshot path
    snprintf(gdata->sspath, sizeof(gdata->sspath),
        "%sscreenshots%c", gdata->datapath, SEP);
}

#ifdef JGRF_STATIC
void jgrf_paths_set_corelib(void) {
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();
    snprintf(gdata->corelib, sizeof(gdata->corelib), "%s", "Statically linked");
    snprintf(gdata->corename, sizeof(gdata->corename), "%s",
        jg_get_coreinfo("")->name);
}

void jgrf_paths_set_coreassets(void) {
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();
    snprintf(gdata->coreassets, sizeof(gdata->coreassets), "%s",
        gdata->binpath);
#if defined(DATADIR) // Check for core assets system-wide
    char coreassets[256];
    snprintf(coreassets, sizeof(coreassets),
        "%s%cjollygood%c%s", DATADIR, SEP, SEP, gdata->corename);

    struct stat fbuf;
    if (stat(coreassets, &fbuf) == 0)
        snprintf(gdata->coreassets, sizeof(gdata->coreassets), "%s",
            coreassets);
#endif // defined(DATADIR)
}

#else

void jgrf_paths_set_corelib(void) {
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Detect the default core for the system
    if (jgrf_cli_core()) {
        snprintf(gdata->corename, sizeof(gdata->corename), "%s",
            jgrf_cli_core());
    }
    else if (!jgrf_detect_core())
        jgrf_log(JG_LOG_ERR,
            "Cannot detect usable core, or invalid file. Exiting...\n");

    // Set the core path to the local core path
    snprintf(gdata->corelib, sizeof(gdata->corelib), "%s%ccores%c%s%c%s.%s",
        gdata->binpath, SEP, SEP, gdata->corename, SEP, gdata->corename, SOEXT);

    // Check if a core exists at that path
    struct stat fbuf;
    int corefound = 0;
    if (stat(gdata->corelib, &fbuf) == 0) {
        corefound = 1;
    }
#if defined(LIBDIR) && defined(DATADIR) // Check for the core system-wide
    else {
        snprintf(gdata->corelib, sizeof(gdata->corelib), "%s%cjollygood%c%s.%s",
            LIBDIR, SEP, SEP, gdata->corename, SOEXT);

        if (stat(gdata->corelib, &fbuf) == 0)
            corefound = 1;
    }
#endif // defined(LIBDIR) && defined(DATADIR)

    /* If the core couldn't be loaded, try a backup core for this system until
       the list is exhausted.
    */
    if (!corefound) {
        if (jgrf_cli_core())
            jgrf_log(JG_LOG_ERR, "Failed to locate core. Exiting...\n");
        else
            jgrf_paths_set_corelib();
    }
}

void jgrf_paths_set_coreassets(void) {
    jgrf_gdata_t *gdata = jgrf_gdata_ptr();
    int assetsfound = 0;
    struct stat fbuf;

    snprintf(gdata->coreassets, sizeof(gdata->coreassets), "%s%ccores%c%s",
        gdata->binpath, SEP, SEP, gdata->corename);

    if (stat(gdata->coreassets, &fbuf) == 0) {
        assetsfound = 1;
    }
#if defined(LIBDIR) && defined(DATADIR) // Check for the core system-wide
    else {
        snprintf(gdata->coreassets, sizeof(gdata->coreassets),
            "%s%cjollygood%c%s", DATADIR, SEP, SEP, gdata->corename);

        if (stat(gdata->coreassets, &fbuf) == 0)
            assetsfound = 1;
    }
#endif // defined(LIBDIR) && defined(DATADIR)

    /* Not all cores have assets, so empty the core asset path and assume the
       core will handle any missing assets accordingly.
    */
    if (!assetsfound)
        snprintf(gdata->coreassets, sizeof(gdata->coreassets), "%s", "");
}

#endif // JGRF_STATIC

int jgrf_paths_env_corelib(void) {
#ifdef JGRF_STATIC
    return 0;
#endif
    if (!getenv("JOLLYGOOD_CORE_DIRS"))
        return 0;

    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Detect the default core for the system
    if (jgrf_cli_core()) {
        snprintf(gdata->corename, sizeof(gdata->corename), "%s",
            jgrf_cli_core());
    }
    else if (!jgrf_detect_core())
        jgrf_log(JG_LOG_ERR,
            "Cannot detect default core, or invalid file. Exiting...\n");

    // Allocate enough memory for the entire env var + null terminator
    size_t len = strlen(getenv("JOLLYGOOD_CORE_DIRS")) + 1;
    char *dirs = calloc(len, sizeof(char));
    snprintf(dirs, len, "%s", getenv("JOLLYGOOD_CORE_DIRS"));

    char *delim = PATHSEP;
    char *token = strtok(dirs, delim);
    char corelib[256];
    struct stat fbuf;
    int corefound = 0;

    while (token) {
        // Check for the core in this path
        snprintf(corelib, sizeof(corelib), "%s%c%s.%s",
            token, SEP, gdata->corename, SOEXT);

        token = strtok(NULL, delim);

        if (stat(corelib, &fbuf) == 0) {
            snprintf(gdata->corelib, sizeof(gdata->corelib), "%s", corelib);
            corefound = 1;
            break;
        }
    }

    // Cleanup
    free(dirs);

    return corefound;
}

int jgrf_paths_env_coreassets(void) {
    if (!getenv("JOLLYGOOD_ASSET_DIRS"))
        return 0;

    jgrf_gdata_t *gdata = jgrf_gdata_ptr();

    // Allocate enough memory for the entire env var + null terminator
    size_t len = strlen(getenv("JOLLYGOOD_ASSET_DIRS")) + 1;
    char *dirs = calloc(len, sizeof(char));
    snprintf(dirs, len, "%s", getenv("JOLLYGOOD_ASSET_DIRS"));

    char *delim = PATHSEP;
    char *token = strtok(dirs, delim);
    struct stat fbuf;
    int assetsfound = 0;

    /* Check if the directory exists -- since asset filenames are unknown at
       this stage, assume the first existing directory in the list contains
       assets the core is able to use.
    */
    while (token) {
        snprintf(gdata->coreassets, sizeof(gdata->coreassets), "%s%c%s",
            token, SEP, gdata->corename);
        token = strtok(NULL, delim);
        if (stat(gdata->coreassets, &fbuf) == 0) {
            assetsfound = 1;
            break;
        }
    }

    // Cleanup
    free(dirs);

    return assetsfound;
}
