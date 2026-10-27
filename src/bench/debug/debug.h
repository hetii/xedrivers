/* Commands for looking under the hood; see debug.c. */

#ifndef XE_DEBUG_CMDS_H
#define XE_DEBUG_CMDS_H

#include "kernel.h"

u32 debug_peek(u32 address, u32 little);
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

#endif /* XE_DEBUG_CMDS_H */
