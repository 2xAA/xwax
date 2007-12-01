/* 
 * Copyright (C) 2007 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#ifndef TIMECODER_H
#define TIMECODER_H

#include "device.h"

#define TIMECODER_CHANNELS DEVICE_CHANNELS
#define TIMECODER_RATE DEVICE_RATE


struct timecoder_channel_t {
    signed int zero, signal_level, half_peak, wave_peak, ref_level;
};

struct timecoder_t {
    struct timecoder_channel_t state[TIMECODER_CHANNELS];

    int positive, /* wave is in positive part of cycle */
        forwards;

    /* Signal levels */

    signed int zero, signal_level, half_peak, wave_peak, ref_level;

    /* Pitch information */

    int crossings, /* number of zero crossings */
        crossings_ticker, /* number of samples from which crossings counted */
        cycle_ticker; /* samples since wave last crossed zero */

    /* Numerical timecode */

    unsigned int bitstream, /* actual bits from the record */
        timecode; /* corrected timecode */
    int valid_counter, /* number of successful error checks */
        timecode_ticker; /* samples since valid timecode was read */

    /* Feedback */

    unsigned char *mon; /* x-y array */
    int mon_size, mon_counter, mon_scale,
        log_fd; /* optional file descriptor to log to, or -1 for none */
};


/* Building the lookup table is global. Need a good way to share
 * lookup tables soon, so we can use a different timecode on 
 * each timecoder, and switch between them. */

int timecoder_build_lookup(char *timecode_name);
void timecoder_free_lookup(void);

void timecoder_init(struct timecoder_t *tc);
void timecoder_clear(struct timecoder_t *tc);

void timecoder_monitor_init(struct timecoder_t *tc, int size, int scale);
void timecoder_monitor_clear(struct timecoder_t *tc);

int timecoder_submit(struct timecoder_t *tc, signed short *aud, int samples);

int timecoder_get_pitch(struct timecoder_t *tc, float *pitch);
signed int timecoder_get_position(struct timecoder_t *tc, int *when);
int timecoder_get_alive(struct timecoder_t *tc);
unsigned int timecoder_get_safe(struct timecoder_t *tc);
int timecoder_get_resolution(struct timecoder_t *tc);

#endif
