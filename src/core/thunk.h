/* A module's import thunk -- `lis r11,hi; addi r11,r11,lo; mtctr r11; bctr` -- read and
 * pointed elsewhere. This is how the plugin puts itself between a module (xam) and a kernel
 * export without touching the kernel: the original stays where it was, and the thunk can be
 * put back. A thunk address is per build: check it decodes to what it should before changing
 * it (thunk_target), as xnet.c and speakers.c do.
 */

#ifndef XE_THUNK_H
#define XE_THUNK_H

#include "kernel.h"

/* Where a thunk's `lis/addi` pair leads. */
u32  thunk_target(u32 lis, u32 addi);

/* Where the thunk at `at` leads now; 0 if it is not mapped. */
u32  thunk_reads(u32 at);

/* Point the thunk at `at` to `target`; its two words before are kept in saved[2]. */
void thunk_set(u32 at, void *target, u32 *saved);

/* Put back what thunk_set saved. */
void thunk_restore(u32 at, const u32 *saved);

#endif /* XE_THUNK_H */
