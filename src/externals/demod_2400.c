// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// demod_2400.c: 2.4MHz Mode S demodulator.
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

/**
 * \file demod_2400.c
 * \ingroup Main
 * \brief A 2.4 MBit/s sampler for Dump1090.
 */

#include "misc.h"

typedef enum {
   SDR_NONE = 0,
   SDR_IFILE,
   SDR_RTLSDR,
 } sdr_type_t;

#define PREAMBLE_THRESHOLD_PIZERO 75
#define MODES_SHORT_MSG_BYTES      7
#define MODES_LONG_MSG_BYTES      14
#define MODES_LONG_MSG_BITS       (MODES_LONG_MSG_BYTES  * 8)
#define MODES_SHORT_MSG_BITS      (MODES_SHORT_MSG_BYTES * 8)

#define MODES_MAX_BITERRORS        2
#define PING_BUCKETS              20 // statistics on round trip time
#define NUM_TYPES                 14

int scoreModesMessage (unsigned char *msg, int validbits);

struct stats
{
  int64_t start;
  int64_t end;
  // Mode S demodulator counts:
  uint32_t demod_preambles;
  uint32_t demod_rejected_bad;
  uint32_t demod_rejected_unknown_icao;
  uint32_t demod_accepted[MODES_MAX_BITERRORS + 1];
  uint32_t demod_preamblePhase[5];
  uint32_t demod_bestPhase[5];
  uint64_t samples_processed;
  uint64_t samples_dropped;
  uint64_t samples_lost;
  // Mode A/C demodulator counts:
  uint32_t demod_modeac;
  // number of signals with power > -3dBFS
  uint32_t strong_signal_count;
  // noise floor:
  double noise_power_sum;
  uint64_t noise_power_count;
  // mean signal power:
  double signal_power_sum;
  uint64_t signal_power_count;
  // peak signal power seen
  double peak_signal_power;
  // timing:
  struct timespec demod_cpu;
  struct timespec reader_cpu;
  struct timespec background_cpu;
  struct timespec aircraft_json_cpu;
  struct timespec trace_json_cpu;
  struct timespec globe_json_cpu;
  struct timespec bin_cpu;
  struct timespec heatmap_and_state_cpu;
  struct timespec remove_stale_cpu;
  struct timespec api_worker_cpu;
  struct timespec api_update_cpu;
  uint64_t api_request_count;
  // remote messages:
  uint32_t remote_received_modeac;
  uint32_t remote_received_modes;
  uint32_t remote_received_basestation_valid;
  uint32_t remote_received_basestation_invalid;
  uint32_t remote_rejected_bad;
  uint32_t remote_rejected_unknown_icao;
  uint32_t remote_rejected_delayed;
  uint32_t remote_accepted[MODES_MAX_BITERRORS + 1];
  uint32_t remote_malformed_beast;
  uint32_t remote_ping_rtt[PING_BUCKETS];
  uint64_t network_bytes_in;
  uint64_t network_bytes_out;
  // total messages:
  uint32_t messages_total;
  // CPR decoding:
  uint32_t cpr_surface;
  uint32_t cpr_airborne;
  uint32_t cpr_global_ok;
  uint32_t cpr_global_bad;
  uint32_t cpr_global_skipped;
  uint32_t cpr_global_range_checks;
  uint32_t cpr_global_speed_checks;
  uint32_t cpr_local_ok;
  uint32_t cpr_local_skipped;
  uint32_t cpr_local_range_checks;
  uint32_t cpr_local_speed_checks;
  uint32_t cpr_local_aircraft_relative;
  uint32_t cpr_local_receiver_relative;
  uint32_t cpr_filtered;

  uint32_t pos_all;
  uint32_t pos_duplicate;
  uint32_t pos_garbage;
  uint32_t pos_by_type[NUM_TYPES];

  uint32_t recentTraceWrites;
  uint32_t fullTraceWrites;
  uint32_t permTraceWrites;

  // number of altitude messages ignored because
  // we had a recent DF17/18 altitude
  uint32_t suppressed_altitude_messages;
  // aircraft:
  // total "new" aircraft (i.e. not seen in the last 30 or 300s)
  uint32_t unique_aircraft;
  // we saw only a single message
  uint32_t single_message_aircraft;
  // range histogram
  uint32_t range_histogram[128];
  double distance_max; // Longest range decoded, in *metres*
  double distance_min; // Shortest range decoded, in *metres*
};


struct readsb_Modes {
    int8_t       nfix_crc;     // Number of crc bit error(s) to correct
    int8_t       fixDF;        // fix message type single bit errors that become DF17
    sdr_type_t   sdr_type;     // where are we getting data from?
    uint32_t     preambleThreshold;
    struct messageBuffer *netMessageBuffer;

    struct stats stats_current;
    struct stats stats_15min;


};

static struct readsb_Modes extra_Modes;

#define Modes extra_Modes


// 2.4MHz sampling rate version
//
// When sampling at 2.4MHz we have exactly 6 samples per 5 symbols.
// Each symbol is 500ns wide, each sample is 416.7ns wide
//
// We maintain a phase offset that is expressed in units of 1/5 of a sample i.e. 1/6 of a symbol, 83.333ns
// Each symbol we process advances the phase offset by 6 i.e. 6/5 of a sample, 500ns
//
// The correlation functions below correlate a 1-0 pair of symbols (i.e. manchester encoded 1 bit)
// starting at the given sample, and assuming that the symbol starts at a fixed 0-5 phase offset within
// m[0]. They return a correlation value, generally interpreted as >0 = 1 bit, <0 = 0 bit

// TODO check if there are better (or more balanced) correlation functions to use here

// nb: the correlation functions sum to zero, so we do not need to adjust for the DC offset in the input signal
// (adding any constant value to all of m[0..3] does not change the result)


// Changes 2020 by wiedehopf:
// 20 units per sample, 24 units per symbol that are distributed according to phase
// 1 bit has 2 symbols, in a bit representing a one the first symbol is high and the second is low

// The previous assumption was that symbols beyond our control are zero.
// Let's make the assumption that the symbols beyond our control are a statistical mean of 0 and 1.
// Such a mean is represented by 12 units per symbol.
// As an example for the above let's discuss the first slice function:
// Samples 0 and 1 are completely occupied by the bit we are trying to judge thus no outside symbols.
// The 3rd sample is 8 units of our bit and 12 units of the following symbol.
// Our bit contributes part of a low symbol represented by -8 units
// but we also get 12 units of 0.5 resulting in +6 units from the following symbol.
//
// The above comment is how these changes started out, i'll leave them here as food for thought.
// Using --ifile the coefficients from the above thought process were iteratively tweaked by hand.
// Note one of the correlation functions is no longer DC balanced (but just slightly)
// Further testing on your own samples using --ifile --quiet --stats is welcome
// Note you might need to use --throttle unless your using wiedehopf's readsb fork,
// otherwise position stats won't work as they rely on realtime differences between
// reception of CPRs.
// Creating a 5 minute sample with a gain of 43.9:
// timeout 300 rtl_sdr -f 1090000000 -s 2400000 -g 43.9 sample.dat
// Checking a set of correlation functions using the above sample:
// make && ./readsb --device-type ifile --ifile sample.dat --quiet --stats

static __inline int slice_phase0(uint16_t *m) {
    return 18 * m[0] - 15 * m[1] - 3 * m[2];
}

static __inline int slice_phase1 (uint16_t *m)
{
  return 14 * m[0] - 5 * m[1] - 9 * m[2];
}

//
// Slightly DC unbalanced but better results
//
static __inline int slice_phase2 (uint16_t *m)
{
  return 16 * m[0] + 5 * m[1] - 20 * m[2];
}

static __inline int slice_phase3 (uint16_t *m)
{
  return 7 * m[0] + 11 * m[1] - 18 * m[2];
}

static __inline int slice_phase4 (uint16_t *m)
{
  return 4 * m[0] + 15 * m[1] - 20 * m[2] + 1 * m[3];
}

static __inline int64_t imin (int64_t a, int64_t b)
{
  if (a < b)
     return (a);
  return (b);
}

static __inline int64_t imax (int64_t a, int64_t b)
{
  if (a > b)
     return (a);
  return (b);
}

//
// datafield extraction helpers
//
// The first bit (MSB of the first byte) is numbered 1, for consistency
// with how the specs number them.
//
// Extract one bit from a message.
//
static __inline unsigned getbit (unsigned char *data, unsigned bitnum)
{
  unsigned bi = bitnum - 1;
  unsigned by = bi >> 3;
  unsigned mask = 1 << (7 - (bi & 7));

  return (data[by] & mask) != 0;
}

//
// Extract some bits (firstbit .. lastbit inclusive) from a message.
//
static __inline unsigned getbits (unsigned char *data, unsigned firstbit, unsigned lastbit)
{
  unsigned fbi = firstbit - 1;
  unsigned lbi = lastbit - 1;
  unsigned nbi = (lastbit - firstbit + 1);
  unsigned fby = fbi >> 3;
  unsigned lby = lbi >> 3;
  unsigned nby = (lby - fby) + 1;
  unsigned shift = 7 - (lbi & 7);
  unsigned topmask = 0xFF >> (fbi & 7);

  assert (fbi <= lbi);
  assert (nbi <= 32);
  assert (nby <= 5);

  if (nby == 5)
  {
    return ((data[fby] & topmask) << (32 - shift)) |
            (data[fby + 1] << (24 - shift)) |
            (data[fby + 2] << (16 - shift)) |
            (data[fby + 3] << (8 - shift)) |
            (data[fby + 4] >> shift);
  }
  else if (nby == 4)
  {
    return ((data[fby] & topmask) << (24 - shift)) |
            (data[fby + 1] << (16 - shift)) |
            (data[fby + 2] << (8 - shift)) |
            (data[fby + 3] >> shift);
  }
  else if (nby == 3)
  {
    return ((data[fby] & topmask) << (16 - shift)) |
            (data[fby + 1] << (8 - shift)) |
            (data[fby + 2] >> shift);
  }
  else if (nby == 2)
  {
    return ((data[fby] & topmask) << (8 - shift)) |
            (data[fby + 1] >> shift);
  }
  else if (nby == 1)
  {
    return (data[fby] & topmask) >> shift;
  }
  else
    return 0;
}

//
// Given the Downlink Format (DF) of the message, return the message length in bits.
//
// All known DF's 16 or greater are long. All known DF's 15 or less are short.
// There are lots of unused codes in both category, so we can assume ICAO will stick to
// these rules, meaning that the most significant bit of the DF indicates the length.
//
static __inline int modesMessageLenByType (int type)
{
  return (type & 0x10) ? MODES_LONG_MSG_BITS : MODES_SHORT_MSG_BITS;
}

static uint32_t valid_df_short_bitset;        // set of acceptable DF values for short messages
static uint32_t valid_df_long_bitset;         // set of acceptable DF values for long messages

static uint32_t generate_damage_set(uint8_t df, unsigned damage_bits)
{
    uint32_t result = (1 << df);
    if (!damage_bits)
        return result;

    for (unsigned bit = 0; bit < 5; ++bit) {
        unsigned damaged_df = df ^ (1 << bit);
        result |= generate_damage_set(damaged_df, damage_bits - 1);
    }

    return result;
}

static void init_bitsets()
{
    // DFs that we directly understand without correction
    valid_df_short_bitset = (1 << 0) | (1 << 4) | (1 << 5) | (1 << 11);
    valid_df_long_bitset = (1 << 16) | (1 << 17) | (1 << 18) | (1 << 20) | (1 << 21);

#ifdef ENABLE_DF24
    if (1)
        valid_df_long_bitset |= (1 << 24) | (1 << 25) | (1 << 26) | (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30) | (1 << 31);
#endif

    // if we can also repair DF damage, include those corrections
    if (Modes.fixDF && Modes.nfix_crc) {
        // only correct for possible DF17, other types are less useful usually (DF11/18 would also be possible)
        valid_df_long_bitset |= generate_damage_set(17, 1);
    }
}


// extract one byte from the mag buffers using slice_phase functions
// advance pPtr and phase
static __inline uint8_t slice_byte(uint16_t **pPtr, int *phase) {
    uint8_t theByte = 0;

    switch (*phase) {
        case 0:
            theByte =
                (slice_phase0(*pPtr) > 0 ? 0x80 : 0) |
                (slice_phase2(*pPtr+2) > 0 ? 0x40 : 0) |
                (slice_phase4(*pPtr+4) > 0 ? 0x20 : 0) |
                (slice_phase1(*pPtr+7) > 0 ? 0x10 : 0) |
                (slice_phase3(*pPtr+9) > 0 ? 0x08 : 0) |
                (slice_phase0(*pPtr+12) > 0 ? 0x04 : 0) |
                (slice_phase2(*pPtr+14) > 0 ? 0x02 : 0) |
                (slice_phase4(*pPtr+16) > 0 ? 0x01 : 0);

            *phase = 1;
            *pPtr += 19;
            break;

        case 1:
            theByte =
                (slice_phase1(*pPtr) > 0 ? 0x80 : 0) |
                (slice_phase3(*pPtr+2) > 0 ? 0x40 : 0) |
                (slice_phase0(*pPtr+5) > 0 ? 0x20 : 0) |
                (slice_phase2(*pPtr+7) > 0 ? 0x10 : 0) |
                (slice_phase4(*pPtr+9) > 0 ? 0x08 : 0) |
                (slice_phase1(*pPtr+12) > 0 ? 0x04 : 0) |
                (slice_phase3(*pPtr+14) > 0 ? 0x02 : 0) |
                (slice_phase0(*pPtr+17) > 0 ? 0x01 : 0);

            *phase = 2;
            *pPtr += 19;
            break;

        case 2:
            theByte =
                (slice_phase2(*pPtr) > 0 ? 0x80 : 0) |
                (slice_phase4(*pPtr+2) > 0 ? 0x40 : 0) |
                (slice_phase1(*pPtr+5) > 0 ? 0x20 : 0) |
                (slice_phase3(*pPtr+7) > 0 ? 0x10 : 0) |
                (slice_phase0(*pPtr+10) > 0 ? 0x08 : 0) |
                (slice_phase2(*pPtr+12) > 0 ? 0x04 : 0) |
                (slice_phase4(*pPtr+14) > 0 ? 0x02 : 0) |
                (slice_phase1(*pPtr+17) > 0 ? 0x01 : 0);

            *phase = 3;
            *pPtr += 19;
            break;

        case 3:
            theByte =
                (slice_phase3(*pPtr) > 0 ? 0x80 : 0) |
                (slice_phase0(*pPtr+3) > 0 ? 0x40 : 0) |
                (slice_phase2(*pPtr+5) > 0 ? 0x20 : 0) |
                (slice_phase4(*pPtr+7) > 0 ? 0x10 : 0) |
                (slice_phase1(*pPtr+10) > 0 ? 0x08 : 0) |
                (slice_phase3(*pPtr+12) > 0 ? 0x04 : 0) |
                (slice_phase0(*pPtr+15) > 0 ? 0x02 : 0) |
                (slice_phase2(*pPtr+17) > 0 ? 0x01 : 0);

            *phase = 4;
            *pPtr += 19;
            break;

        case 4:
            theByte =
                (slice_phase4(*pPtr) > 0 ? 0x80 : 0) |
                (slice_phase1(*pPtr+3) > 0 ? 0x40 : 0) |
                (slice_phase3(*pPtr+5) > 0 ? 0x20 : 0) |
                (slice_phase0(*pPtr+8) > 0 ? 0x10 : 0) |
                (slice_phase2(*pPtr+10) > 0 ? 0x08 : 0) |
                (slice_phase4(*pPtr+12) > 0 ? 0x04 : 0) |
                (slice_phase1(*pPtr+15) > 0 ? 0x02 : 0) |
                (slice_phase3(*pPtr+17) > 0 ? 0x01 : 0);

            *phase = 0;
            *pPtr += 20;
            break;
    }
    return theByte;
}

static void score_phase(int try_phase, uint16_t *pa, unsigned char **bestmsg, int *bestscore, int *bestphase, unsigned char **msg, unsigned char *msg1, unsigned char *msg2) {
    Modes.stats_current.demod_preamblePhase[try_phase - 4]++;
    uint16_t *pPtr;
    int phase, score, bytelen;

    pPtr = pa + 19 + (try_phase / 5);
    phase = try_phase % 5;

    (*msg)[0] = slice_byte(&pPtr, &phase);

    // inspect DF field early, only continue processing
    // messages where the DF appears valid
    uint32_t df = ((uint8_t) (*msg)[0]) >> 3;
    if (valid_df_long_bitset & (1 << df)) {
        bytelen = MODES_LONG_MSG_BYTES;
    } else if (valid_df_short_bitset & (1 << df)) {
        bytelen = MODES_SHORT_MSG_BYTES;
    } else {
        score = -2;
        if (score > *bestscore) {
            // this is only for preamble stats
            *bestscore = score;
        }
        return;
    }

    for (int i = 1; i < bytelen; ++i) {
        (*msg)[i] = slice_byte(&pPtr, &phase);
    }

    // Score the mode S message and see if it's any good.
    score = scoreModesMessage(*msg, bytelen * 8);
    if (score > *bestscore) {
        // new high score!
        *bestmsg = *msg;
        *bestscore = score;
        *bestphase = try_phase;

        // swap to using the other buffer so we don't clobber our demodulated data
        // (if we find a better result then we'll swap back, but that's OK because
        // we no longer need this copy if we found a better one)
        *msg = (*msg == msg1) ? msg2 : msg1;
    }
}

//
// Given 'mlen' magnitude samples in 'm', sampled at 2.4MHz,
// try to demodulate some Mode S messages.
//
void demodulate2400(struct mag_buf *mag) {
    u_char msg1[MODES_LONG_MSG_BYTES];
    u_char msg2[MODES_LONG_MSG_BYTES];
    u_char *msg;

    unsigned char *bestmsg = NULL;
    int bestscore;
    int bestphase = 0;

    uint16_t *m = mag->data;
    uint32_t mlen = mag->length;

    uint64_t sum_scaled_signal_power = 0;

    // initialize bitsets on first call
    if (!valid_df_short_bitset)
        init_bitsets();

    msg = msg1;

    uint16_t *pa = m;
    uint16_t *stop = m + mlen;

    for (; pa < stop; pa++) {
        int32_t pa_mag, base_noise, ref_level;
        int msglen;

        // Look for a message starting at around sample 0 with phase offset 3..7

        // Ideal sample values for preambles with different phase
        // Xn is the first data symbol with phase offset N
        //
        // sample#: 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
        // phase 3: 2/4\0/5\1 0 0 0 0/5\1/3 3\0 0 0 0 0 0 X4
        // phase 4: 1/5\0/4\2 0 0 0 0/4\2 2/4\0 0 0 0 0 0 0 X0
        // phase 5: 0/5\1/3 3\0 0 0 0/3 3\1/5\0 0 0 0 0 0 0 X1
        // phase 6: 0/4\2 2/4\0 0 0 0 2/4\0/5\1 0 0 0 0 0 0 X2
        // phase 7: 0/3 3\1/5\0 0 0 0 1/5\0/4\2 0 0 0 0 0 0 X3

        // do a pre-check to reduce CPU usage
        // some silly unrolling that cuts CPU cycles
        // due to plenty room in the message buffer for decoding
        // we can with pa go beyond stop without a buffer overrun ...

        if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }
        pa++; if (pa[1] > pa[7] && pa[12] > pa[14] && pa[12] > pa[15]) { goto after_pre; }

        continue;

after_pre:
        // ... but we must NOT decode if have ran past stop
        if (!(pa < stop))
            continue;

        // 5 noise samples
        base_noise = pa[5] + pa[8] + pa[16] + pa[17] + pa[18];
        // pa_mag is the sum of the 4 preamble high bits
        // minus 2 low bits between each of high bit pairs

        // reduce number of preamble detections if we recently dropped samples
        if (Modes.stats_15min.samples_dropped)
            ref_level = base_noise * imax(PREAMBLE_THRESHOLD_PIZERO, Modes.preambleThreshold);
        else
            ref_level = base_noise * Modes.preambleThreshold;

        ref_level >>= 5; // divide by 32

        bestscore = -42;

        int32_t diff_2_3 =  pa[2] - pa[3];
        int32_t sum_1_4 = pa[1] + pa[4];
        int32_t diff_10_11 = pa[10] - pa[11];
        int32_t common3456 = sum_1_4 - diff_2_3 + pa[9] + pa[12];

        // sample#: 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
        // phase 3: 2/4\0/5\1 0 0 0 0/5\1/3 3\0 0 0 0 0 0 X4
        // phase 4: 1/5\0/4\2 0 0 0 0/4\2 2/4\0 0 0 0 0 0 0 X0
        pa_mag = common3456 - diff_10_11;
        if (pa_mag >= ref_level) {
            // peaks at 1,3,9,11-12: phase 3
            score_phase(4, pa, &bestmsg, &bestscore, &bestphase, &msg, msg1, msg2);

            // peaks at 1,3,9,12: phase 4
            score_phase(5, pa, &bestmsg, &bestscore, &bestphase, &msg, msg1, msg2);
        }

        // sample#: 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
        // phase 5: 0/5\1/3 3\0 0 0 0/3 3\1/5\0 0 0 0 0 0 0 X1
        // phase 6: 0/4\2 2/4\0 0 0 0 2/4\0/5\1 0 0 0 0 0 0 X2
        pa_mag = common3456 + diff_10_11;
        if (pa_mag >= ref_level) {
            // peaks at 1,3-4,9-10,12: phase 5
            score_phase(6, pa, &bestmsg, &bestscore, &bestphase, &msg, msg1, msg2);

            // peaks at 1,4,10,12: phase 6
            score_phase(7, pa, &bestmsg, &bestscore, &bestphase, &msg, msg1, msg2);
        }

        // peaks at 1-2,4,10,12: phase 7
        // sample#: 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
        // phase 7: 0/3 3\1/5\0 0 0 0 1/5\0/4\2 0 0 0 0 0 0 X3
        pa_mag = sum_1_4 + 2 * diff_2_3 + diff_10_11 + pa[12];
        if (pa_mag >= ref_level)
            score_phase(8, pa, &bestmsg, &bestscore, &bestphase, &msg, msg1, msg2);


        // no preamble detected
        if (bestscore == -42)
            continue;

        // we had at least one phase greater than the preamble threshold
        // and used scoremodesmessage on those bytes
        Modes.stats_current.demod_preambles++;

        // Do we have a candidate?
        if (bestscore < 0) {

            if (bestscore == -1)
                Modes.stats_current.demod_rejected_unknown_icao++;
            else
                Modes.stats_current.demod_rejected_bad++;
            continue; // nope.
        }

        msglen = modesMessageLenByType(getbits(bestmsg, 1, 5));

        struct modesMessage *mm = netGetMM(&Modes.netMessageBuffer[0]);

        // For consistency with how the Beast / Radarcape does it,
        // we report the timestamp at the end of bit 56 (even if
        // the frame is a 112-bit frame)
        mm->timestamp = mag->sampleTimestamp + (pa -m) * 5 + (8 + 56) * 12 + bestphase;

        // compute message receive time as block-start-time + difference in the 12MHz clock
        mm->sysTimestamp = mag->sysTimestamp + receiveclock_ms_elapsed(mag->sampleTimestamp, mm->timestamp);

        mm->score = bestscore;

        // Decode the received message
        {
            memcpy(mm->msg, bestmsg, MODES_LONG_MSG_BYTES);
            int result = decodeModesMessage(mm);
            if (result < 0) {
                if (result == -1)
                    Modes.stats_current.demod_rejected_unknown_icao++;
                else
                    Modes.stats_current.demod_rejected_bad++;
                continue;
            } else {
                Modes.stats_current.demod_accepted[mm->correctedbits]++;
            }
        }

        Modes.stats_current.demod_bestPhase[bestphase - 4]++;

        // measure signal power
        {
            double signal_power;
            uint64_t scaled_signal_power = 0;
            int signal_len = msglen * 12 / 5;
            int k;

            for (k = 0; k < signal_len; ++k) {
                uint32_t mag = pa[19 + k];
                scaled_signal_power += mag * mag;
            }

            signal_power = scaled_signal_power / 65535.0 / 65535.0;
            mm->signalLevel = signal_power / signal_len;
            Modes.stats_current.signal_power_sum += signal_power;
            Modes.stats_current.signal_power_count += signal_len;
            sum_scaled_signal_power += scaled_signal_power;

            if (mm->signalLevel > Modes.stats_current.peak_signal_power)
                Modes.stats_current.peak_signal_power = mm->signalLevel;
            if (mm->signalLevel > 0.50119)
                Modes.stats_current.strong_signal_count++; // signal power above -3dBFS
        }

        // Skip over the message:
        // (we actually skip to 8 bits before the end of the message,
        //  because we can often decode two messages that *almost* collide,
        //  where the preamble of the second message clobbered the last
        //  few bits of the first message, but the message bits didn't
        //  overlap)
        //pa += msglen * 12 / 5;
        //
        // let's test something, only jump part of the message and let the preamble detection handle the rest.
        pa += msglen * 8 / 4;

        // Pass data to the next layer
        netUseMessage(mm);
    }

    /* update noise power */
    {
        double sum_signal_power = sum_scaled_signal_power / 65535.0 / 65535.0;
        Modes.stats_current.noise_power_sum += (mag->mean_power * mag->length - sum_signal_power);
        Modes.stats_current.noise_power_count += mag->length;
    }

    netDrainMessageBuffers();
}

#if 0

//////////
////////// MODE A/C
//////////

// Mode A/C bits are 1.45us wide, consisting of 0.45us on and 1.0us off
// We track this in terms of a (virtual) 60MHz clock, which is the lowest common multiple
// of the bit frequency and the 2.4MHz sampling frequency
//
//            0.45us = 27 cycles }
//            1.00us = 60 cycles } one bit period = 1.45us = 87 cycles
//
// one 2.4MHz sample = 25 cycles
void demodulate2400AC(struct mag_buf *mag) {
    uint16_t *m = mag->data;
    uint32_t mlen = mag->length;
    unsigned f1_sample;

    double noise_stddev = sqrt(mag->mean_power - mag->mean_level * mag->mean_level); // Var(X) = E[(X-E[X])^2] = E[X^2] - (E[X])^2
    unsigned noise_level = (unsigned) ((mag->mean_power + noise_stddev) * 65535 + 0.5);

    for (f1_sample = 1; f1_sample < mlen; ++f1_sample) {
        // Mode A/C messages should match this bit sequence:

        // bit #     value
        //   -1       0    quiet zone
        //    0       1    framing pulse (F1)
        //    1      C1
        //    2      A1
        //    3      C2
        //    4      A2
        //    5      C4
        //    6      A4
        //    7       0    quiet zone (X1)
        //    8      B1
        //    9      D1
        //   10      B2
        //   11      D2
        //   12      B4
        //   13      D4
        //   14       1    framing pulse (F2)
        //   15       0    quiet zone (X2)
        //   16       0    quiet zone (X3)
        //   17     SPI
        //   18       0    quiet zone (X4)
        //   19       0    quiet zone (X5)

        // Look for a F1 and F2 pair,
        // with F1 starting at offset f1_sample.

        // the first framing pulse covers 3.5 samples:
        //
        // |----|        |----|
        // | F1 |________| C1 |_
        //
        // | 0 | 1 | 2 | 3 | 4 |
        //
        // and there is some unknown phase offset of the
        // leading edge e.g.:
        //
        //   |----|        |----|
        // __| F1 |________| C1 |_
        //
        // | 0 | 1 | 2 | 3 | 4 |
        //
        // in theory the "on" period can straddle 3 samples
        // but it's not a big deal as at most 4% of the power
        // is in the third sample.

        if (!(m[f1_sample - 1] < m[f1_sample + 0]))
            continue; // not a rising edge

        if (m[f1_sample + 2] > m[f1_sample + 0] || m[f1_sample + 2] > m[f1_sample + 1])
            continue; // quiet part of bit wasn't sufficiently quiet

        unsigned f1_level = (m[f1_sample + 0] + m[f1_sample + 1]) / 2;

        if (noise_level * 2 > f1_level) {
            // require 6dB above noise
            continue;
        }

        // estimate initial clock phase based on the amount of power
        // that ended up in the second sample

        float f1a_power = (float) m[f1_sample] * m[f1_sample];
        float f1b_power = (float) m[f1_sample + 1] * m[f1_sample + 1];
        float fraction = f1b_power / (f1a_power + f1b_power);
        unsigned f1_clock = (unsigned) (25 * (f1_sample + fraction * fraction) + 0.5);

        // same again for F2
        // F2 is 20.3us / 14 bit periods after F1
        unsigned f2_clock = f1_clock + (87 * 14);
        unsigned f2_sample = f2_clock / 25;
        assert(f2_sample < mlen + Modes.trailing_samples);

        if (!(m[f2_sample - 1] < m[f2_sample + 0]))
            continue;

        if (m[f2_sample + 2] > m[f2_sample + 0] || m[f2_sample + 2] > m[f2_sample + 1])
            continue; // quiet part of bit wasn't sufficiently quiet

        unsigned f2_level = (m[f2_sample + 0] + m[f2_sample + 1]) / 2;

        if (noise_level * 2 > f2_level) {
            // require 6dB above noise
            continue;
        }

        unsigned f1f2_level = (f1_level > f2_level ? f1_level : f2_level);

        float midpoint = sqrtf(noise_level * f1f2_level); // geometric mean of the two levels
        unsigned signal_threshold = (unsigned) (midpoint * M_SQRT2 + 0.5); // +3dB
        unsigned noise_threshold = (unsigned) (midpoint / M_SQRT2 + 0.5); // -3dB

        // Looks like a real signal. Demodulate all the bits.
        unsigned uncertain_bits = 0;
        unsigned noisy_bits = 0;
        unsigned bits = 0;
        unsigned bit;
        unsigned clock;
        for (bit = 0, clock = f1_clock; bit < 20; ++bit, clock += 87) {
            unsigned sample = clock / 25;

            bits <<= 1;
            noisy_bits <<= 1;
            uncertain_bits <<= 1;

            // check for excessive noise in the quiet period
            if (m[sample + 2] >= signal_threshold) {
                noisy_bits |= 1;
            }

            // decide if this bit is on or off
            if (m[sample + 0] >= signal_threshold || m[sample + 1] >= signal_threshold) {
                bits |= 1;
            } else if (m[sample + 0] > noise_threshold && m[sample + 1] > noise_threshold) {
                /* not certain about this bit */
                uncertain_bits |= 1;
            } else {
                /* this bit is off */
            }
        }

        // framing bits must be on
        if ((bits & 0x80020) != 0x80020) {
            continue;
        }

        // quiet bits must be off
        if ((bits & 0x0101B) != 0) {
            continue;
        }

        if (noisy_bits || uncertain_bits) {
            continue;
        }

        // Convert to the form that we use elsewhere:
        //  00 A4 A2 A1  00 B4 B2 B1  SPI C4 C2 C1  00 D4 D2 D1
        unsigned modeac =
                ((bits & 0x40000) ? 0x0010 : 0) | // C1
                ((bits & 0x20000) ? 0x1000 : 0) | // A1
                ((bits & 0x10000) ? 0x0020 : 0) | // C2
                ((bits & 0x08000) ? 0x2000 : 0) | // A2
                ((bits & 0x04000) ? 0x0040 : 0) | // C4
                ((bits & 0x02000) ? 0x4000 : 0) | // A4
                ((bits & 0x00800) ? 0x0100 : 0) | // B1
                ((bits & 0x00400) ? 0x0001 : 0) | // D1
                ((bits & 0x00200) ? 0x0200 : 0) | // B2
                ((bits & 0x00100) ? 0x0002 : 0) | // D2
                ((bits & 0x00080) ? 0x0400 : 0) | // B4
                ((bits & 0x00040) ? 0x0004 : 0) | // D4
                ((bits & 0x00004) ? 0x0080 : 0); // SPI

        // This message looks good, submit it
        struct modesMessage *mm = netGetMM(&Modes.netMessageBuffer[0]);

        // For consistency with how the Beast / Radarcape does it,
        // we report the timestamp at the second framing pulse (F2)
        mm->timestamp = mag->sampleTimestamp + f2_clock / 5; // 60MHz -> 12MHz

        // compute message receive time as block-start-time + difference in the 12MHz clock
        mm->sysTimestamp = mag->sysTimestamp + receiveclock_ms_elapsed(mag->sampleTimestamp, mm->timestamp);

        decodeModeAMessage(mm, modeac);

        // Pass data to the next layer
        netUseMessage(mm);

        f1_sample += (20 * 87 / 25);
        Modes.stats_current.demod_modeac++;
    }

    netDrainMessageBuffers();
}
#endif
