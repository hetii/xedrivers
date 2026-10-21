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

#endif /* XE_DEBUG_CMDS_H */
