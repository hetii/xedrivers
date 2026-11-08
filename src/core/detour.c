/* See detour.h. */

#include "detour.h"

/* lis r?,hi / ori r?,r?,lo with the register in `r`. */
static void load(u32 *at, u32 r, u32 value)
{
    at[0] = 0x3C000000u | r << 21 | (value >> 16);              /* addis r,0,hi */
    at[1] = 0x60000000u | r << 21 | r << 16 | (value & 0xFFFFu); /* ori   r,r,lo */
}

u32 detour_install(DETOUR *d, void *function, void *entry)
{
    volatile u32 *f = (volatile u32 *)function;
    u32 n = 0, i, back = (u32)(unsigned long)function + 16;

    if (d->installed || !MmIsAddressValid(function))
        return 0;
    for (i = 0; i < 4; i++) {
        u32 w = f[i], op = w >> 26;

        if (op == 18 && (w & 3u) == 1u) {               /* bl: a relative call, made absolute */
            s32 off = (s32)(w << 6) >> 6 & ~3;
            u32 target = (u32)(unsigned long)(f + i) + (u32)off;

            load(d->code + n, 0, target);
            d->code[n + 2] = 0x7C0903A6u;               /* mtctr r0 */
            d->code[n + 3] = 0x4E800421u;               /* bctrl    */
            n += 4;
        } else if (op == 16 || op == 18 || op == 19) {  /* any other branch: not moved */
            return 0;
        } else {
            d->code[n++] = w;
        }
        d->saved[i] = w;
    }
    load(d->code + n, 0, back);
    d->code[n + 2] = 0x7C0903A6u;                       /* mtctr r0 */
    d->code[n + 3] = 0x4E800420u;                       /* bctr     */
    KeSweepIcacheRange(d->code, sizeof(d->code));
    d->at = (u32)(unsigned long)function;

    /* The jump: lis r11 / addi r11 / mtctr r11 / bctr, r11 being free at a call's start.
     * First the first word becomes `b .`, so that a caller arriving meanwhile waits there
     * rather than run half of each; then the other three; then the first. */
    {
        u32 a = (u32)(unsigned long)entry, hi = (a + 0x8000u) >> 16;

        f[0] = 0x48000000u;                             /* b . */
        KeSweepIcacheRange((void *)f, 4);
        f[1] = 0x396B0000u | (a & 0xFFFFu);             /* addi r11,r11,lo */
        f[2] = 0x7D6903A6u;                             /* mtctr r11 */
        f[3] = 0x4E800420u;                             /* bctr */
        KeSweepIcacheRange((void *)f, 16);
        f[0] = 0x3D600000u | (hi & 0xFFFFu);            /* lis r11,hi */
        KeSweepIcacheRange((void *)f, 16);
    }
    d->installed = 1;
    return 1;
}

void detour_remove(DETOUR *d)
{
    volatile u32 *f = (volatile u32 *)(unsigned long)d->at;
    u32 i;

    if (!d->installed)
        return;
    f[0] = 0x48000000u;                                 /* b . while the rest goes back */
    KeSweepIcacheRange((void *)f, 4);
    for (i = 1; i < 4; i++)
        f[i] = d->saved[i];
    KeSweepIcacheRange((void *)f, 16);
    f[0] = d->saved[0];
    KeSweepIcacheRange((void *)f, 16);
    d->installed = 0;
}
