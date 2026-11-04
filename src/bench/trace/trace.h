/* The recorder that survives the console; see trace.c. */

#ifndef XE_TRACE_H
#define XE_TRACE_H

#include "kernel.h"

void trace_event(u32 code, u32 a, u32 b, u32 c);
void trace_tick(u32 tick);

#endif /* XE_TRACE_H */
