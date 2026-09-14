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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <speex/speex_resampler.h>

#include "jgrf.h"
#include "audio.h"
#include "cli.h"
#include "input.h"
#include "settings.h"
#include "wave_writer.h"

// Divergence tolerance between current and average input samples per frame
#define SPFTOLERANCE 28

static jgrf_gdata_t *gdata = NULL;

// Pointer to audio information from the emulator core
static jg_audioinfo_t *audinfo = NULL;

// Info about audio to be input to the core (via microphone)
static jg_audioinfo_t micinfo = { JG_SAMPFMT_INT16, 48000, 1, 800, NULL };

// Audio Output
static SDL_AudioStream *audiostream = NULL;

// Audio Input
static SDL_AudioStream *audiostream_in = NULL;

// Speex Resampler
static SpeexResamplerState *resampler = NULL;
static int err;

// Wave Writer
static wave_writer *ww;
static wave_writer_format wwformat;
static wave_writer_error wwerror;

// Buffers for audio samples
static void *corebuf = NULL; // Audio buffer for the core
static int16_t *rsbuf = NULL; // Buffer for audio data to be resampled
static int16_t *outbuf = NULL; // Buffer for final output samples

static mavg_t mavg_in = { 0, 0, {0} }; // Moving Average for input sizes
static ringbuf_t rbuf_in = { 0, 0, 0, 0, NULL }; // Ring buffer (input audio)
static struct timespec req, rem;

static int ma_offset = 0; // Offset the input moving average chunk size

static int mute = 0;

extern int waveout; // Wave File Output

// Add a new sample chunk size to the moving average and recalculate
static inline void jgrf_audio_mavg(mavg_t *mavg, size_t newval) {
    mavg->buf[mavg->pos++] = newval;

    if (mavg->pos == MAVGSIZE)
        mavg->pos = 0;

    double sum = 0;

    for (uint32_t i = 0; i < MAVGSIZE; ++i)
        sum += mavg->buf[i];

    double divisor = MAVGSIZE;
    mavg->avg = sum / divisor;
}

// Seed the moving average input sample chunk size
static inline void jgrf_audio_mavg_seed(mavg_t *mavg, size_t seed) {
    for (uint32_t i = 0; i < MAVGSIZE; ++i)
        mavg->buf[i] = seed;
    jgrf_audio_mavg(mavg, seed);
}

// Enqueue and Dequeue samples
static inline int16_t jgrf_rbuf_deq(ringbuf_t *rbuf) {
    if (rbuf->cursize == 0)
        return 0;

    int16_t sample = rbuf->buffer[rbuf->head];
    rbuf->head = (rbuf->head + 1) % rbuf->bufsize;
    --rbuf->cursize;
    return sample;
}

static inline void jgrf_rbuf_enq(ringbuf_t *rbuf, int16_t *data, size_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        rbuf->buffer[rbuf->tail] = data[i];
        rbuf->tail = (rbuf->tail + 1) % rbuf->bufsize;
        ++rbuf->cursize;
        if (rbuf->cursize >= rbuf->bufsize - 1)
            break;
    }
}

static inline void jgrf_rbuf_enqf(ringbuf_t *rbuf, float *data, size_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        data[i] *= 32768.0;

        if (data[i] >= 32767.0)
            rbuf->buffer[rbuf->tail] = 32767;
        else if (data[i] <= -32768.0)
            rbuf->buffer[rbuf->tail] = -32768;
        else
            rbuf->buffer[rbuf->tail] = (int16_t)(lrintf(data[i]));

        rbuf->tail = (rbuf->tail + 1) % rbuf->bufsize;
        ++rbuf->cursize;
        if (rbuf->cursize >= rbuf->bufsize - 1)
            break;
    }
}

// Set timing information to align audio processing with core framerate
void jgrf_audio_timing(double frametime) {
    int spf = (audinfo->rate / (int)(frametime + 0.5)) * audinfo->channels;

    if (abs(spf - (int)(mavg_in.avg + 0.5)) >= SPFTOLERANCE) {
        int oldavg = (int)mavg_in.avg;
        jgrf_audio_mavg_seed(&mavg_in, spf);
        jgrf_log(JG_LOG_DBG,
            "Moving Averge set: %f fps, %d spf (old: %d, diff: %d)\n",
            frametime, spf, oldavg, abs(spf - oldavg));
    }
}

// Poll audio capture and push to core
/*static void jgrf_audio_capture_poll(void) {
    if (!audiostream_in)
        return;

    int avail = SDL_GetAudioStreamAvailable(audiostream_in);
    if (avail <= 0)
        return;

    static int16_t capbuf[4096];
    int toread = avail;
    if (toread > (int)sizeof(capbuf))
        toread = sizeof(capbuf);

    int got = SDL_GetAudioStreamData(audiostream_in, capbuf, toread);
    if (got > 0) {
        micinfo.buf = (void*)capbuf;
        jgrf_data_push(JG_DATA_AUDIO, 0, &micinfo, got / sizeof(int16_t));
    }
}*/

// Callback used by core to tell the frontend how many samples are ready
void jgrf_audio_cb_core(void *udata, size_t in_size) {
    (void)udata;
    /* If Benchmark mode is set, or the core calls this function with no
       samples ready, do nothing.
    */
    if (bmark || !in_size)
        return;

    // Adjust input moving average calculation to reflect this input size
    jgrf_audio_mavg(&mavg_in, in_size);

    // Keep the output queue aligned to size of the ideal samples per frame
    size_t spf = (audinfo->rate / corefps) * audinfo->channels;

    // Input moving average samples per frame - make sure the number is even
    uint32_t ma_insamps =
        (uint32_t)(mavg_in.avg) + ((uint32_t)(mavg_in.avg) % 2);

    // Control the size of the input queue
    if (rbuf_in.cursize == 0) // Buffer ~half samples if empty
        ma_offset = -((in_size >> 1) + ((in_size >> 1) % audinfo->channels));
    else if (rbuf_in.cursize < spf)
        ma_offset = -audinfo->channels; // Eat Less
    else if (rbuf_in.cursize > (spf + (spf >> 2)))
        ma_offset = audinfo->channels; // Eat More
    else if (rbuf_in.cursize == spf)
        ma_offset = 0; // Goldilocks Zone

    ma_insamps += ma_offset;

    // Calculate rates for use in resampling
    uint32_t in_rate = (ma_insamps * corefps) / audinfo->channels;
    uint32_t out_rate = audinfo->rate / (fforward ? fforward + 1 : 1);

    /* Use SDL's audio stream queue depth as the feedback signal.
       Target 3-4 frames worth of samples in the SDL stream buffer.
    */
    int qbytes = SDL_GetAudioStreamAvailable(audiostream);
    int qsamps = qbytes / sizeof(int16_t);
    out_rate += (4 - (qsamps / spf)) * screenfps;

    // Change the resampling ratio
    err = speex_resampler_set_rate_frac(resampler,
        in_rate, out_rate, in_rate, out_rate);

    // Debug output
    //jgrf_log(JG_LOG_DBG, "i - r: %d, s: %d, c: %d, ma: %d (%f), fps: %d\n",
    //  in_rate, in_size, rbuf_in.cursize, ma_insamps, mavg_in.avg, corefps);

    uint32_t out_size = (ma_insamps * (double)out_rate) / (double)in_rate;

    // Enqueue samples from the core for resampling and output
    if (audinfo->sampfmt == JG_SAMPFMT_INT16)
        jgrf_rbuf_enq(&rbuf_in, audinfo->buf, in_size);
    else
        jgrf_rbuf_enqf(&rbuf_in, audinfo->buf, in_size);

    // Dequeue the moving average number of samples to be resampled
    for (uint32_t i = 0; i < ma_insamps; ++i)
        rsbuf[i] = jgrf_rbuf_deq(&rbuf_in);

    // Perform resampling
    if (audinfo->channels == 2) {
        ma_insamps >>= 1;
        out_size >>= 1;

        err = speex_resampler_process_interleaved_int(resampler,
            mute ? NULL : rsbuf, &ma_insamps, outbuf, &out_size);

        out_size <<= 1;
    }
    else {
        err = speex_resampler_process_int(resampler, 0,
            mute ? NULL : rsbuf, &ma_insamps, outbuf, &out_size);
    }

    while ((SDL_GetAudioStreamAvailable(audiostream) / sizeof(int16_t)) +
        out_size >= (audinfo->spf * 6))
        nanosleep(&req, &rem);

    // Push resampled audio directly into the SDL AudioStream
    SDL_PutAudioStreamData(audiostream, outbuf, out_size * sizeof(int16_t));

    //jgrf_log(JG_LOG_DBG, "o - r: %d, s: %d, c: %d\n",
    //  out_rate, out_size,
    //  (SDL_GetAudioStreamAvailable(audiostream) / sizeof(int16_t)));

    // Wave file output
    if (waveout)
        wave_writer_put_samples(ww, out_size / audinfo->channels, outbuf);
}

// Pass the core's audio information into the frontend
void jgrf_audio_set_info(jg_audioinfo_t *ptr) {
    audinfo = ptr;
}

// Initialize the audio device and allocate buffers
int jgrf_audio_init(void) {
    jg_setting_t *settings = jgrf_settings_ptr();
    gdata = jgrf_gdata_ptr();

    // Set up Wave Writer
    if (waveout) {
        wwformat.num_channels = audinfo->channels;
        wwformat.sample_rate = audinfo->rate;
        wwformat.sample_bits = 16;
        ww = wave_writer_open(jgrf_cli_wave(), &wwformat, &wwerror);
    }

    SDL_AudioSpec audiospec;
    audiospec.channels = audinfo->channels;
    audiospec.freq = audinfo->rate;
    audiospec.format = SDL_AUDIO_S16;

    // Open an audio stream bound to the default playback device (no callback)
    audiostream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audiospec, NULL, NULL);

    // Set up Audio Capture
    if (gdata->hints & JG_HINT_INPUT_AUDIO) {
        SDL_AudioSpec capspec;
        capspec.channels = micinfo.channels;
        capspec.freq = micinfo.rate;
        capspec.format = SDL_AUDIO_S16;

        audiostream_in = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &capspec, NULL, NULL);
    }

    // Set up the Resampler
    resampler = speex_resampler_init(audinfo->channels,
        audinfo->rate, audinfo->rate, settings[AUDIO_RSQUAL].val, &err);

    if (audiostream && resampler)
        jgrf_log(JG_LOG_INF, "Audio: %dHz %s, Speex %d\n", audiospec.freq,
            audiospec.channels == 1 ? "Mono" : "Stereo",
            settings[AUDIO_RSQUAL].val);
    else
        jgrf_log(JG_LOG_WRN, "Audio: Error opening audio device.\n");

    if (audiostream_in)
        jgrf_log(JG_LOG_INF, "Audio Capture: Enabled\n");

    // Seed the moving averages
    jgrf_audio_mavg_seed(&mavg_in, audinfo->spf);

    if (!corefps)
        corefps = (audinfo->rate / audinfo->spf) * audinfo->channels;

    // Set delay time in nanoseconds
    req.tv_nsec = 100000; // 1/10th of a millisecond

    // Allocate audio buffers
    size_t bufsize = audinfo->spf * sizeof(int16_t) * 4;

    if (!(gdata->hints & JG_HINT_AUDIO_INTERNAL))
        corebuf = (void*)calloc(1, bufsize << audinfo->sampfmt);

    rsbuf = (void*)calloc(1, bufsize);
    outbuf = (void*)calloc(1, bufsize * 2);

    size_t rbufsize = audinfo->spf * 6;
    rbuf_in.bufsize = rbufsize;
    rbuf_in.buffer = (void*)calloc(1, rbufsize * sizeof(int16_t));

    // Pass the core audio buffer into the emulator if needed
    audinfo->buf = corebuf;

    return 1;
}

// Unpause the audio device
void jgrf_audio_unpause(void) {
    if (audiostream)
        SDL_ResumeAudioStreamDevice(audiostream);
    if (audiostream_in)
        SDL_ResumeAudioStreamDevice(audiostream_in);
}

// Deinitialize the audio device and free buffers
void jgrf_audio_deinit(void) {
    if (waveout) wave_writer_close(ww, &wwerror);
    if (audiostream_in) SDL_DestroyAudioStream(audiostream_in);
    if (audiostream) SDL_DestroyAudioStream(audiostream);
    if (resampler) speex_resampler_destroy(resampler);

    if (!(gdata->hints & JG_HINT_AUDIO_INTERNAL))
        if (corebuf) free(corebuf);

    if (rbuf_in.buffer) free(rbuf_in.buffer);
    if (rsbuf) free(rsbuf);
    if (outbuf) free(outbuf);
}

// Toggle audio playback (mute/unmute)
void jgrf_audio_toggle(void) {
    mute ^= 1;
    jgrf_log(JG_LOG_SCR, "Audio %s", mute ? "Muted" : "Unmuted");
}
