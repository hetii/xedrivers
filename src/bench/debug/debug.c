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
