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

#ifndef AUDIO_H
#define AUDIO_H

#define MAVGSIZE 60 * 2 // 60 * N second moving average

// Moving Average
typedef struct mavg_t {
    double avg; // The moving average
    uint32_t pos; // Position in the buffer
    size_t buf[MAVGSIZE]; // The buffer
} mavg_t;

// Ring Buffer (Audio Queue)
typedef struct ringbuf_t {
    uint32_t head; // Head position
    uint32_t tail; // Tail position
    uint32_t cursize; // Current queue size
    uint32_t bufsize; // Buffer size
    int16_t *buffer; // Pointer to the buffer
} ringbuf_t;

void jgrf_audio_set_info(jg_audioinfo_t*);
void jgrf_audio_timing(double);
void jgrf_audio_cb_core(void*, size_t);
void jgrf_audio_unpause(void);
int jgrf_audio_init(void);
void jgrf_audio_deinit(void);
void jgrf_audio_toggle(void);

#endif
