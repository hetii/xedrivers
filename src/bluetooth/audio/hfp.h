/* HFP 1.8, the console as the audio gateway: a headset's microphone for the voice chat and its
 * earpiece for what comes back. The service level connection over RFCOMM (hfp.c), then the
 * synchronous (SCO) link for the voice itself (sco.c, later).
 */

#ifndef XE_HFP_H
#define XE_HFP_H

#include "rfcomm.h"

#define HFP_CHANNEL 1u          /* our RFCOMM server channel, in the SDP record */

typedef struct {
    u32 state;                  /* 0 none, 1 RFCOMM open, 2 service level up, 3 audio */
    u32 hf_features;            /* AT+BRSF from the headset */
    u32 commands;
    u32 unknown;                /* answered ERROR */
    u32 last_unknown[2];        /* its first 8 characters */
    u32 codecs;                 /* AT+BAC: bit n for codec n */
    u32 speaker_gain;           /* AT+VGS */
    u32 mic_gain;               /* AT+VGM */
    u32 sco_asked;
    u32 sco_status;             /* of Synchronous Connection Complete */
    u32 sco_handle;
    u32 sco_air;                /* the air mode it gave: 2 CVSD, 3 transparent */
    u32 sco_transport;          /* what starting its data in the transport gave */
} HFP_STATS;

extern volatile HFP_STATS hfp_stats;

/* Pure (tests/bluetooth): one AT command line (no CR) into the answer, written to `out`;
 * its length. */
u32  hfp_answer(const char *line, u32 n, char *out, u32 room);

void hfp_init(void);
void hfp_tick(void);
void hfp_gone(volatile BT_CONN *c);
void hfp_sco_event(const u8 *event, u32 length);    /* sync complete, disconnection */
u32  hfp_voice(void);           /* ask for the voice link again; 0 if it will be */

#endif /* XE_HFP_H */
