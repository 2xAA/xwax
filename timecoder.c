/*
 * Copyright (C) 2008 Mark Hills <mark@pogo.org.uk>
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "timecoder.h"

#define ZERO_THRESHOLD 128
#define SIGNAL_THRESHOLD 256

/* Time constants for the filters */

#define ZERO_RC 0.001
#define SIGNAL_RC 0.004

#define REF_PEAKS_AVG 48 /* in wave cycles */

/* The number of correct bits which come in before the timecode 
 * is declared valid. Set this too low, and risk the record skipping around 
 * (often to blank areas of track) during scratching */

#define VALID_BITS 24

#define MONITOR_DECAY_EVERY 512 /* in samples */

#define SQ(x) ((x)*(x))


/* Timecode definitions */


#define POLARITY_NEGATIVE 0
#define POLARITY_POSITIVE 1

struct timecode_def_t {
    char *name, *desc;
    int bits, /* number of bits in string */
        resolution, /* wave cycles per second */
        polarity; /* cycle begins POLARITY_POSITIVE or POLARITY_NEGATIVE */
    bits_t seed, /* LFSR value at timecode zero */
        taps; /* central LFSR taps, excluding end taps */
    unsigned int length, /* in cycles */
        safe; /* last 'safe' timecode number (for auto disconnect) */
    signed int *lookup; /* pointer to built lookup table */
};


struct timecode_def_t timecode_def[] = {
    {
        name: "serato_2a",
        desc: "Serato 2nd Ed., side A",
        resolution: 1000,
        polarity: POLARITY_POSITIVE,
        bits: 20,
        seed: 0x59017,
        taps: 0x361e4,
        length: 712000,
        safe: 707000,
        lookup: NULL
    },
    {
        name: "serato_2b",
        desc: "Serato 2nd Ed., side B",
        resolution: 1000,
        polarity: POLARITY_POSITIVE,
        bits: 20,
        seed: 0x8f3c6,
        taps: 0x4f0d8, /* reverse of side A */
        length: 922000,
        safe: 917000,
        lookup: NULL
    },
    {
        name: "serato_cd",
        desc: "Serato CD",
        resolution: 1000,
        polarity: POLARITY_POSITIVE,
        bits: 20,
        seed: 0x84c0c,
        taps: 0x34d54,
        length: 940000,
        safe: 930000,
        lookup: NULL
    },
    {
        name: "traktor_a",
        desc: "Traktor Scratch, side A",
        resolution: 2000,
        polarity: POLARITY_POSITIVE,
        bits: 23,
        seed: 0x134503,
        taps: 0x041040,
        length: 1500000,
        safe: 1480000,
        lookup: NULL
    },
    {
        name: "traktor_b",
        desc: "Traktor Scratch, side B",
        resolution: 2000,
        polarity: POLARITY_POSITIVE,
        bits: 23,
        seed: 0x32066c,
        taps: 0x041040, /* same as side A */
        length: 2110000,
        safe: 2090000,
        lookup: NULL
    },
    {
        name: NULL
    }
};


struct timecode_def_t *def;


/* Linear Feeback Shift Register in the forward direction. New values
 * are generated at the least-significant bit. */

static inline bits_t lfsr(bits_t code, bits_t taps)
{
    bits_t taken;
    int xrs;

    taken = code & taps;
    xrs = 0;
    while (taken != 0x0) {
        xrs += taken & 0x1;
        taken >>= 1;
    }

    return xrs & 0x1;
}


static inline bits_t fwd(bits_t current, struct timecode_def_t *def)
{
    bits_t l;

    /* New bits are added at the MSB; shift right by one */

    l = lfsr(current, def->taps | 0x1);
    return (current >> 1) | (l << (def->bits - 1));
}


static inline bits_t rev(bits_t current, struct timecode_def_t *def)
{
    bits_t l, mask;

    /* New bits are added at the LSB; shift left one and mask */

    mask = (1 << def->bits) - 1;
    l = lfsr(current, (def->taps >> 1) | (0x1 << (def->bits - 1)));
    return ((current << 1) & mask) | l;
}


/* Setup globally, for a chosen timecode definition */

int timecoder_build_lookup(char *timecode_name) {
    unsigned int n;
    bits_t current, last;

    def = &timecode_def[0];

    while(def->name) {
        if(!strcmp(def->name, timecode_name))
            break;
        def++;
    }

    if(!def->name) {
        fprintf(stderr, "Timecode definition '%s' is not known.\n",
                timecode_name);
        return -1;
    }

    fprintf(stderr, "Allocating %d slots (%zuKb) for %d bit timecode (%s)\n",
            2 << def->bits, (2 << def->bits) * sizeof(unsigned int) / 1024,
            def->bits, def->desc);

    def->lookup = malloc((2 << def->bits) * sizeof(unsigned int));
    if(!def->lookup) {
        perror("malloc");
        return 0;
    }
    
    for(n = 0; n < ((unsigned int)2 << def->bits); n++)
        def->lookup[n] = -1;
    
    current = def->seed;
    
    for(n = 0; n < def->length; n++) {
        if(def->lookup[current] != -1) {
            fprintf(stderr, "Timecode has wrapped; finishing here.\n");
            return -1;
        }
        
        def->lookup[current] = n;
        last = current;
        current = fwd(current, def);
        assert(rev(current, def) == last);
    }
    
    return 0;    
}


/* Free the timecoder lookup table when it is no longer needed */

void timecoder_free_lookup(void) {
    free(def->lookup);
}


static void init_channel(struct timecoder_channel_t *ch)
{
    ch->positive = 0;
    ch->zero = 0;
    ch->crossing_ticker = 0;
}


/* Initialise a timecode decoder */

void timecoder_init(struct timecoder_t *tc)
{
    int c;

    tc->forwards = 1;
    tc->rate = 0;

    tc->half_peak = 0;
    tc->wave_peak = 0;
    tc->ref_level = -1;
    tc->signal_level = 0;

    init_channel(&tc->mono);
    for(c = 0; c < TIMECODER_CHANNELS; c++)
        init_channel(&tc->channel[c]);
        
    tc->crossings = 0;
    tc->pitch_ticker = 0;

    tc->bitstream = 0;
    tc->timecode = 0;
    tc->valid_counter = 0;
    tc->timecode_ticker = 0;

    tc->mon = NULL;
    tc->log_fd = -1;
}


/* Clear a timecode decoder */

void timecoder_clear(struct timecoder_t *tc)
{
    timecoder_monitor_clear(tc);
}


/* The monitor (otherwise known as 'scope' in the interface) is the
 * display of the incoming audio. Initialise one for the given
 * timecoder */

void timecoder_monitor_init(struct timecoder_t *tc, int size)
{
    tc->mon_size = size;
    tc->mon = malloc(SQ(tc->mon_size));
    memset(tc->mon, 0, SQ(tc->mon_size));
    tc->mon_counter = 0;
}


/* Clear the monitor on the given timecoder */

void timecoder_monitor_clear(struct timecoder_t *tc)
{
    if(tc->mon) {
        free(tc->mon);
        tc->mon = NULL;
    }
}


static void set_sample_rate(struct timecoder_t *tc, int rate)
{
    float dt;

    tc->rate = rate;

    /* Pre-calculate the alpha values for the filters */

    dt = 1.0 / rate;
    tc->zero_alpha = dt / (ZERO_RC + dt);
    tc->signal_alpha = dt / (SIGNAL_RC + dt);
}


static int detect_zero_crossing(struct timecoder_channel_t *ch,
                                signed short v, float alpha)
{
    int swapped;

    ch->crossing_ticker++;

    swapped = 0;
    if(v >= ch->zero + ZERO_THRESHOLD && !ch->positive) {
        swapped = 1;
        ch->positive = 1;
        ch->crossing_ticker = 0;
    } else if(v < ch->zero - ZERO_THRESHOLD && ch->positive) {
        swapped = 1;
        ch->positive = 0;
        ch->crossing_ticker = 0;
    }
    
    ch->zero += alpha * (v - ch->zero);
    
    return swapped;
}


/* Submit and decode a block of PCM audio data to the timecoder */

int timecoder_submit(struct timecoder_t *tc, signed short *pcm,
		     int samples, int rate)
{
    int s, c,
        x, y, p, /* monitor coordinates */
        offset,
        swapped,
        monitor_centre;
    signed int g, m; /* pcm sample value, sum of two short channels */
    float v, w;
    bits_t b, l, /* bitstream and timecode bits */
	mask;

    set_sample_rate(tc, rate);

    b = 0;
    l = 0;
    
    mask = ((1 << def->bits) - 1);
    monitor_centre = tc->mon_size / 2;

    offset = 0;

    for(s = 0; s < samples; s++) {
        
        for(c = 0; c < TIMECODER_CHANNELS; c++) {
            detect_zero_crossing(&tc->channel[c], pcm[offset + c],
				 tc->zero_alpha);
	}

        /* Read from the mono channel */
        
        g = pcm[offset] + pcm[offset + 1];
        swapped = detect_zero_crossing(&tc->mono, g, tc->zero_alpha);

        /* If a sign change in the (zero corrected) audio has
         * happened, log the peak information */
        
        if(swapped) {
            
            /* Work out whether half way through a cycle we are
             * looking for the wave to be positive or negative */
            
            if(tc->mono.positive == (def->polarity ^ tc->forwards)) {
                
                /* Entering the second half of a wave cycle */
                
                tc->half_peak = tc->wave_peak;
                
            } else {
                
                /* Completed a full wave cycle, so time to analyse the
                 * level and work out whether it's a 1 or 0 */
                
                b = tc->wave_peak + tc->half_peak > tc->ref_level;
                
                /* Log binary timecode */
                
                if(tc->log_fd != -1)
                    write(tc->log_fd, b ? "1" : "0", 1);
                
                /* Add it to the bitstream, and work out what we were
                 * expecting (timecode). */
                
                /* tc->bitstream is always in the order it is
                 * physically placed on the vinyl, regardless of the
                 * direction. */
                
                if(tc->forwards) {
                    tc->timecode = fwd(tc->timecode, def);
                    tc->bitstream = (tc->bitstream >> 1)
                        + (b << (def->bits - 1));

                } else {
                    tc->timecode = rev(tc->timecode, def);
                    tc->bitstream = ((tc->bitstream << 1) & mask) + b;
                }
                
                if(tc->timecode == tc->bitstream)
                    tc->valid_counter++;
                else {
                    tc->timecode = tc->bitstream;
                    tc->valid_counter = 0;
                }
                
                /* Take note of the last time we read a valid timecode */
                
                tc->timecode_ticker = 0;
                
                /* Adjust the reference level based on the peaks seen
                 * in this cycle */
                
                if(tc->ref_level == -1)
                    tc->ref_level = tc->half_peak + tc->wave_peak;
                else {
                    tc->ref_level = (tc->ref_level * (REF_PEAKS_AVG - 1)
                                     + tc->half_peak + tc->wave_peak)
                        / REF_PEAKS_AVG;
                }
                
            }
            
            /* Calculate the immediate direction from phase difference,
             * based on the last channel to cross zero */

            if(tc->channel[0].crossing_ticker > tc->channel[1].crossing_ticker)
                tc->forwards = 1;
            else
                tc->forwards = 0;

            if(tc->forwards)
                tc->crossings++;
            else
                tc->crossings--;
            
            tc->pitch_ticker += tc->crossing_ticker;
            tc->crossing_ticker = 0;
            tc->wave_peak = 0;
            
        } /* swapped */
        
        tc->crossing_ticker++;
        tc->timecode_ticker++;
        
        /* Find the zero-normalised sample of the peak value from
         * the input */
        
        m = abs(g - tc->mono.zero);
        if(m > tc->wave_peak)
            tc->wave_peak = m;
        
        /* Take a rolling average of zero and signal level */

	tc->signal_level += tc->signal_alpha * (m - tc->signal_level);

        /* Update the monitor to add the incoming sample */
        
        if(tc->mon) {
            
            /* Decay the pixels already in the montior */
            
            if(++tc->mon_counter % MONITOR_DECAY_EVERY == 0) {
                for(p = 0; p < SQ(tc->mon_size); p++) {
                    if(tc->mon[p])
                        tc->mon[p] = tc->mon[p] * 7 / 8;
                }
            }
            
            v = (float)pcm[offset] / tc->ref_level; /* first channel */
            w = (float)pcm[offset + 1] / tc->ref_level; /* second channel */
            
            x = monitor_centre + (v * tc->mon_size);
            y = monitor_centre + (w * tc->mon_size);
            
            /* Set the pixel value to white */
            
            if(x > 0 && x < tc->mon_size && y > 0 && y < tc->mon_size)
                tc->mon[y * tc->mon_size + x] = 0xff;
        }
        
        offset += TIMECODER_CHANNELS;
        
    } /* for each sample */
    
    /* Print debugging information */
    
#if 0
    fprintf(stderr, "%+6d +/%4d -/%4d (%4d,%4d)\t= %d (%d) %c %d"
            "\t[crossings: %d %d]",
            tc->mono.zero,
            tc->half_peak,
            tc->wave_peak,
            tc->ref_level >> 1,
            tc->signal_level,
            b, l, b == l ? ' ' : 'x',
            tc->valid_counter,
            tc->crossings,
            tc->pitch_ticker);

    if(tc->pitch_ticker)
        fprintf(stderr, " = %d", tc->rate * tc->crossings / tc->pitch_ticker);

    fputc('\n', stderr);
#endif

    return 0;
}


/* Return the timecode pitch, based on cycles of the sine wave. This
 * function can only be called by one context, at it resets the state
 * of the counter in the timecoder. */

int timecoder_get_pitch(struct timecoder_t *tc, float *pitch)
{
    /* Let the caller know if there's no data to gather pitch from */

    if(tc->crossings == 0)
        return -1;

    /* Value of tc->crossings may be negative in reverse */
    
    *pitch = tc->rate * (float)tc->crossings / tc->pitch_ticker
        / (def->resolution * 2);

    tc->crossings = 0;
    tc->pitch_ticker = 0;

    return 0;
}


/* Return the known position in the timecode, or -1 if not known. If
 * two few bits have been error-checked, then this also counts as
 * invalid. If 'when' is given, return the time, in seconds since this
 * value was read. */

signed int timecoder_get_position(struct timecoder_t *tc, float *when)
{
    signed int r;

    if(tc->valid_counter > VALID_BITS) {
        r = def->lookup[tc->bitstream];

        if(r >= 0) {
            if(when) 
                *when = tc->timecode_ticker / tc->rate;
            return r;
        }
    }
    
    return -1;
}


/* Return non-zero if there is any timecode signal available */

int timecoder_get_alive(struct timecoder_t *tc)
{
    if(tc->signal_level < SIGNAL_THRESHOLD)
        return 0;
    
    return 1;
}


/* Return the last 'safe' timecode value on the record. Beyond this
 * value, we probably want to ignore the timecode values, as we will
 * hit the label of the record. */

unsigned int timecoder_get_safe(struct timecoder_t *tc)
{
    return def->safe;
}


/* Return the resolution of the timecode. This is the number of bits
 * per second, which corresponds to the frequency of the sine wave */

int timecoder_get_resolution(struct timecoder_t *tc)
{
    return def->resolution;
}
