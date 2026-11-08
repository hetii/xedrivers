/* See thunk.h. */

#include "thunk.h"

u32 thunk_target(u32 lis, u32 addi)
{
    u32 hi = lis & 0xFFFF;
    s32 lo = (s16)(addi & 0xFFFF);      /* addi sign-extends its half */

    return (hi << 16) + (u32)lo;
}

u32 thunk_reads(u32 at)
{
    const volatile u32 *p = (const volatile u32 *)(unsigned long)at;

    if (!MmIsAddressValid((const void *)p) || !MmIsAddressValid((const void *)(p + 1)))
        return 0;
    return thunk_target(p[0], p[1]);
}

void thunk_set(u32 at, void *target, u32 *saved)
{
    volatile u32 *p = (volatile u32 *)(unsigned long)at;
    u32 a = (u32)(unsigned long)target;
    u32 hi = (a + 0x8000u) >> 16;

    if (saved) {
        saved[0] = p[0];
        saved[1] = p[1];
    }
    p[0] = 0x3D600000u | (hi & 0xFFFF);         /* lis  r11,hi    */
    p[1] = 0x396B0000u | (a & 0xFFFFu);         /* addi r11,r11,lo */
    KeSweepIcacheRange((void *)p, 8);
}

void thunk_restore(u32 at, const u32 *saved)
{
    volatile u32 *p = (volatile u32 *)(unsigned long)at;

    p[0] = saved[0];
    p[1] = saved[1];
    KeSweepIcacheRange((void *)p, 8);
}
