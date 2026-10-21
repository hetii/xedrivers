/* A word anywhere the kernel can reach, device registers included, which XBDM's
 * getmem refuses (the EHCI controllers' registers at 0x7FEA3000 / 0x7FEA5000 are
 * little-endian, hence the byte-reversed forms); and another module's thread at
 * another priority, to measure what a file server's thread scheduling costs it
 * (ftpdll serves a whole transfer from one thread at priority 5).
 */

#include "debug.h"

/* (thread id, &thread) -> NTSTATUS, a referenced KTHREAD; read off ordinals 267,
 * 163 and 261 on 17559. KeSetPriorityThread is absolute and returns the old one. */
extern u32 ObLookupThreadByThreadId(u32 id, void **thread);
extern u32 KeSetPriorityThread(void *thread, u32 priority);
extern void ObDereferenceObject(void *object);

u32 debug_peek(u32 address, u32 little)
{
    u32 v;

#if defined(__powerpc__)
    if (little)
        __asm__ volatile("lwbrx %0, 0, %1; eieio" : "=r"(v) : "r"(address) : "memory");
    else
        __asm__ volatile("lwz %0, 0(%1); eieio" : "=r"(v) : "b"(address) : "memory");
#else
    (void)address;
    (void)little;
    v = 0;
#endif
    return v;
}

u32 debug_poke(u32 address, u32 little, u32 value)
{
#if defined(__powerpc__)
    if (little)
        __asm__ volatile("stwbrx %0, 0, %1; eieio" :: "r"(value), "r"(address) : "memory");
    else
        __asm__ volatile("stw %0, 0(%1); eieio" :: "r"(value), "b"(address) : "memory");
#else
    (void)address;
    (void)little;
    (void)value;
#endif
    return 0;
}

volatile u32 debug_config_value[2];

u32 debug_config(u32 category, u32 setting)
{
    u8 buffer[32];
    u16 needed = 0;
    u32 status, i;

    for (i = 0; i < sizeof(buffer); i++)
        buffer[i] = 0;
    debug_config_value[0] = debug_config_value[1] = 0;
    status = ExGetXConfigSetting((u16)category, (u16)setting, buffer,
                                 (u16)sizeof(buffer), &needed);
    if (!NT_SUCCESS(status))
        return 0;
    for (i = 0; i < 8; i++)
        debug_config_value[i / 4] |= (u32)buffer[i] << (24 - 8 * (i % 4));
    return needed;
}

u32 debug_thread_priority(u32 id, u32 priority)
{
    void *thread = 0;
    u32 status, old;

    status = ObLookupThreadByThreadId(id, &thread);
    if (!NT_SUCCESS(status) || !thread)
        return status;
    old = KeSetPriorityThread(thread, priority);
    ObDereferenceObject(thread);
    return old;
}

/* --- the kernel's cryptography, against known answers -------------------------
 * Three checks, so a wrong calling convention cannot pass unnoticed:
 *   1  SHA-1 of "abc" = a9993e364706816aba3e25717850c26c9cd0d89d (FIPS 180-2).
 *   2  HMAC-SHA1 with a 20-byte key of 0x0b over "Hi There" (RFC 2202 case 1),
 *      given as three pieces, which is how the key derivation uses it.
 *   4  AES-128 of the FIPS-197 block under its key, and the same back again.
 */

volatile u32 debug_crypto_out[12];

static u32 same(const u8 *a, const u8 *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

static void keep(u32 slot, const u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i < n && i < 16; i++)
        debug_crypto_out[slot + i / 4] |= (u32)p[i] << (24 - 8 * (i % 4));
}

u32 debug_crypto(void)
{
    static const u8 sha_abc[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
    static const u8 hmac_want[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
        0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00 };
    static const u8 aes_key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const u8 aes_in[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const u8 aes_want[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    static XE_SHA_STATE sha;
    static XE_HMAC_STATE hmac;
    static XE_AES_STATE aes;
    u8 out[20], back[16], key[20];
    u32 passed = 0, i;

    for (i = 0; i < 12; i++)
        debug_crypto_out[i] = 0;

    XeCryptShaInit(&sha);
    XeCryptShaUpdate(&sha, (const u8 *)"abc", 3);
    XeCryptShaFinal(&sha, out, 20);
    keep(0, out, 16);
    if (same(out, sha_abc, 20))
        passed |= 1;

    for (i = 0; i < 20; i++)
        key[i] = 0x0B;
    for (i = 0; i < 20; i++)
        out[i] = 0;
    XeCryptHmacShaInit(&hmac, key, 20);
    XeCryptHmacShaUpdate(&hmac, (const u8 *)"Hi ", 3);
    XeCryptHmacShaUpdate(&hmac, (const u8 *)"The", 3);
    XeCryptHmacShaUpdate(&hmac, (const u8 *)"re", 2);
    XeCryptHmacShaFinal(&hmac, out, 20);
    keep(4, out, 16);
    if (same(out, hmac_want, 20))
        passed |= 2;

    XeCryptAesKey(&aes, aes_key);
    XeCryptAesEcb(&aes, aes_in, out, 1);
    keep(8, out, 16);
    XeCryptAesEcb(&aes, out, back, 0);
    if (same(out, aes_want, 16) && same(back, aes_in, 16))
        passed |= 4;

    return passed;
}
