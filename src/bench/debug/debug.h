/* Commands for looking under the hood; see debug.c. */

#ifndef XE_DEBUG_CMDS_H
#define XE_DEBUG_CMDS_H

#include "kernel.h"

u32 debug_peek(u32 address, u32 little);
u32 debug_poke(u32 address, u32 little, u32 value);
u32 debug_thread_priority(u32 id, u32 priority);

#endif /* XE_DEBUG_CMDS_H */
