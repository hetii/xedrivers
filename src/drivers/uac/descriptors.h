/* What a USB Audio Class device offers, read from its configuration descriptor:
 * pure code, tested on the host. UAC1 and UAC2 (Audio Device Class 1.0 and 2.0).
 *
 * Only what the console can use is kept: a stereo output and a mono or stereo input
 * in PCM of 16 or 24 bits on isochronous endpoints. UAC1 lists its sample rates in the
 * descriptor; a UAC2 device tells them through its clock source, asked at bring-up, so
 * `rate_listed` stays 0 for it.
 */

#ifndef XE_UAC_DESCRIPTORS_H
#define XE_UAC_DESCRIPTORS_H

#include "kernel.h"

#define UAC_RATE            48000u

#define UAC_SYNC_NONE       0u
#define UAC_SYNC_ASYNC      1u
#define UAC_SYNC_ADAPTIVE   2u
#define UAC_SYNC_SYNC       3u

typedef struct {
    u32 found;
    u32 interface;              /* the AudioStreaming interface */
    u32 alternate;              /* and its setting */
    u32 channels;
    u32 bytes;                  /* per sample in a packet: 2, 3 or 4 */
    u32 bits;                   /* of which carry sound */
    u32 endpoint;               /* address, direction bit included */
    u32 max_packet;
    u32 interval;
    u32 sync;                   /* UAC_SYNC_* */
    u32 feedback;               /* an explicit feedback endpoint's address, or 0 */
    u32 feedback_max_packet;
    u32 feedback_interval;
    u32 rate_listed;            /* UAC1: UAC_RATE is among the rates listed */
    u32 terminal;               /* the terminal this stream links to */
} UAC_STREAM;

typedef struct {
    u32 version;                /* 1 or 2; 0: no Audio Control interface */
    u32 control_interface;
    u32 clock;                  /* UAC2: the first clock source's id */
    UAC_STREAM play;            /* a stereo output */
    UAC_STREAM play6;           /* a 5.1 output: six channels, taken as L R C LFE and the surrounds */
    UAC_STREAM mic;
} UAC_INFO;

/* Read a whole configuration descriptor. 1 if there is anything usable in it. */
u32 uac_parse(const u8 *config, u32 length, UAC_INFO *out);


#endif /* XE_UAC_DESCRIPTORS_H */
