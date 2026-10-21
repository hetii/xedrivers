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

#endif /* XE_DEBUG_CMDS_H */
