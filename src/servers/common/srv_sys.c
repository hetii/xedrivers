/* srv.h's threads, memory, lock, time and settings on the console.
 *
 * Threads: ExCreateThread on a trampoline that first pins the thread and sets the
 * priority data threads need (20, as bench/tcp measured: below 19 xnet's own
 * threads take the quantum).
 */

#include "srv.h"
#include "file.h"

extern void *ExAllocatePoolTypeWithTag(u32 bytes, u32 tag, u32 pool_type);
extern u32 KeSetPriorityThread(void *thread, u32 priority);
extern u32 KeSetAffinityThread(void *thread, u32 affinity, u32 *previous);
extern void KeQuerySystemTime(s64 *time);
extern void ExFreePool(void *p);

/* --- threads ----------------------------------------------------------------- */

#define THREAD_PRIORITY 20
#define THREAD_STACK    0x10000u
#define STARTS          32

/* The hardware thread the servers run on (srv_cpu in the ini, 4 by default): not
 * 0 or 1 (a server pinned there hung the console once, bench/tcp), not 2 (the USB
 * stack's DPCs) and not 5 (the driver's worker). Which one serves best is still
 * to be measured. */
static u32 thread_cpu = 0xFFFFFFFFu;

typedef struct {
    void (*fn)(void *);
    void *arg;
    volatile u32 used;
} START;

static START starts[STARTS];

static inline void *current_thread(void)
{
    void *t;
#if defined(__powerpc__)
    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(t));
#else
    t = 0;
#endif
    return t;
}

static u32 trampoline(void *p)
{
    START *s = (START *)p;
    void (*fn)(void *) = s->fn;
    void *arg = s->arg;
    u32 previous;

    mem_sync();
    s->used = 0;
    KeSetAffinityThread(current_thread(), 1u << thread_cpu, &previous);
    KeSetPriorityThread(current_thread(), THREAD_PRIORITY);
    fn(arg);
    return 0;
}

s32 srv_thread(void (*fn)(void *arg), void *arg)
{
    void *thread = 0;
    u32 i;
    START *s = 0;

    srv_lock();
    for (i = 0; i < STARTS; i++)
        if (!starts[i].used) {
            s = &starts[i];
            s->used = 1;
            break;
        }
    srv_unlock();
    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    mem_sync();
    if (thread_cpu == 0xFFFFFFFFu) {
        u32 cpu = srv_setting_u32("srv_cpu", 4);

        thread_cpu = cpu >= 2 && cpu <= 5 ? cpu : 4;
    }
    if (ExCreateThread(&thread, THREAD_STACK, 0, 0, (void *)trampoline, s, 2) < 0) {
        s->used = 0;
        return -1;
    }
    NtClose(thread);
    return 0;
}

void srv_sleep(u32 ms)
{
    s64 interval = -((s64)ms * 10000);      /* relative, 100 ns units */

    KeDelayExecutionThread(0, 0, &interval);
}

/* --- memory and the lock ------------------------------------------------------ */

void *srv_alloc(u32 bytes)
{
    u8 *p = (u8 *)ExAllocatePoolTypeWithTag(bytes, 0x76727378u /* "xsrv" */, 2);
    u32 i;

    if (p)
        for (i = 0; i < bytes; i++)
            p[i] = 0;
    return p;
}

void srv_free(void *p)
{
    if (p)
        ExFreePool(p);
}

static KSPIN_LOCK lock;
static u32 lock_irql;

void srv_lock(void)
{
    u32 irql = KfAcquireSpinLock(&lock);

    lock_irql = irql;
}

void srv_unlock(void)
{
    KfReleaseSpinLock(&lock, lock_irql);
}

/* 100 ns since 1601 (the kernel's times) into seconds since 1970; srv_file.c
 * uses it too. */
u32 srv_nt_time(s64 t)
{
    u64 x, q = 0, d = 10000000ull << 31;
    u32 i;

    if (t <= 116444736000000000LL)
        return 0;
    x = (u64)(t - 116444736000000000LL);
    for (i = 0; i < 32; i++, d >>= 1) {         /* no 64-bit divide in the plugin */
        q <<= 1;
        if (x >= d) {
            x -= d;
            q |= 1;
        }
    }
    return (u32)q;
}

/* --- time and settings -------------------------------------------------------- */

u32 srv_now(void)
{
    s64 t = 0;

    KeQuerySystemTime(&t);
    return srv_nt_time(t);
}

#define SETTINGS "xedrivers.ini"            /* beside the plugin (src/core/file.h) */

u32 srv_setting(const char *key, char *out, u32 size)
{
    return file_setting(SETTINGS, key, out, size) != 0;
}

u32 srv_setting_u32(const char *key, u32 fallback)
{
    char v[24];
    u32 i, n = 0, base = 10;

    if (!srv_setting(key, v, sizeof(v)))
        return fallback;
    i = 0;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        i = 2;
    }
    for (; v[i]; i++) {
        u32 d = v[i] >= '0' && v[i] <= '9' ? (u32)(v[i] - '0') :
                v[i] >= 'a' && v[i] <= 'f' ? (u32)(v[i] - 'a' + 10) :
                v[i] >= 'A' && v[i] <= 'F' ? (u32)(v[i] - 'A' + 10) : 99;

        if (d >= base)
            break;
        n = n * base + d;
    }
    return n;
}
