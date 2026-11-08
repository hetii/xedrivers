/* A function of another module (xam) entered through ours first: its first four instructions
 * are moved to a trampoline, which runs them and goes on to the fifth, and in their place a
 * jump to our entry is written. The kernel's xam export stays callable through the trampoline.
 *
 * Only for a function whose first four instructions are plain or a `bl` (a prologue's register
 * save, rewritten to an absolute call through r0); anything else that branches, or reads the
 * program counter, is refused. Checked before anything is written.
 */

#ifndef XE_DETOUR_H
#define XE_DETOUR_H

#include "kernel.h"

typedef struct {
    u32 code[16];               /* the trampoline: up to 4 x 4 for the moved ones, then the jump */
    u32 at;                     /* the function */
    u32 saved[4];               /* its first four, to put back */
    u32 installed;
} DETOUR;

/* 1 if `function` now jumps to `entry`; d->code is then the way to the original. */
u32  detour_install(DETOUR *d, void *function, void *entry);
void detour_remove(DETOUR *d);

#endif /* XE_DETOUR_H */
