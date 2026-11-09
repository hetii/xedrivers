/* Commands for looking under the hood; see debug.c. */

#ifndef XE_DEBUG_CMDS_H
#define XE_DEBUG_CMDS_H

#include "kernel.h"

/* What memory the console has: MmQueryStatistics' words, then how many 64 KB blocks can
 * be taken, and given back at once (at most 1024 each): pool types 2 and 1, physical
 * flags 2 and 1 (0 gave the same as 2). */
u32 debug_memory(void);
extern volatile u32 debug_memory_stats[32];
extern volatile u32 debug_memory_blocks[4];
u32 debug_peek(u32 address, u32 little);
u32 debug_pci(u32 bdf, u32 reg, u32 write);
extern volatile u32 debug_pci_value;
u32 debug_poke(u32 address, u32 little, u32 value);
u32 debug_thread_priority(u32 id, u32 priority);

/* One setting of the console's configuration, for finding which one holds what:
 * returns its size, or 0 when the kernel refused it, and leaves the first eight
 * bytes in debug_config_value. */
u32 debug_config(u32 category, u32 setting);
extern volatile u32 debug_config_value[2];

/* Does the kernel's own cryptography answer as it should, called the way
 * src/wireless calls it? Returns a bitmap of the checks that passed, 7 for all
 * three; what each produced is in debug_crypto_out. */
u32 debug_crypto(void);
extern volatile u32 debug_crypto_out[12];

/* WPA2's key from a passphrase and its group-key unwrapping, against the worked
 * examples in IEEE 802.11i and RFC 3394. 3 when both pass. */
u32 debug_wpa(void);

/* A transmit that never finishes: for every network device whose driver keeps
 * NET_STATS.tx_inflight, transfers outstanding with neither that count nor
 * tx_frames moving for two seconds. What was seen then is kept, and the link is
 * reported down so that xnet moves to another uplink -- on the bench the onboard
 * port is cabled, and the console can still be asked about it. Every worker tick. */
typedef struct {
    u32 stalls;
    u32 inflight;           /* outstanding when it was seen */
    u32 tx_frames;
    u32 rx_frames;
    u32 tick;               /* the worker's tick count then */
} DEBUG_TX_STALL;

extern volatile DEBUG_TX_STALL debug_tx_stall[];
void debug_tick(u32 tick);

/* The wireless-card probe (omni_probe.c): every option query (op 0), setting (op 1)
 * and link poll (op 2) the kernel passes to a registered wireless card. */
#define DEBUG_OMNI_LOG 128
typedef struct {
    u32 seq;
    u32 op;
    u32 option;
    u32 length;             /* the buffer's length as asked */
    u32 result;
    u32 repeats;            /* the same call again, right after, not recorded apart */
    u8  data[80];           /* its first bytes after the call */
} DEBUG_OMNI_CALL;

extern volatile DEBUG_OMNI_CALL debug_omni_log[DEBUG_OMNI_LOG];
extern volatile u32 debug_omni_calls;
extern volatile u32 debug_omni_state;
u32 debug_omni_register(void);
u32 debug_omni_unregister(void);
extern volatile u32 debug_omni_wired_hidden;
u32 debug_omni_wired_off(u32 seconds);
void debug_omni_tick(void);

/* A microphone request through xam's XamVoiceSubmitPacket, as XHV makes one: `units` (two
 * 16 kHz samples each, up to 512) into debug_voice_buffer. The packet's words are left in
 * debug_voice_words; returns its status word after up to 200 ms. */
u32 debug_voice(u32 units);
extern volatile u32 debug_voice_words[8];
extern s16 debug_voice_buffer[1024];

#endif /* XE_DEBUG_CMDS_H */
