/* lwIP's operating system layer on the console's kernel: semaphores are the kernel's
 * own, a mailbox is a ring behind one, a thread is ExCreateThread pinned as the file
 * servers' are, the lightweight protection is a critical section (it nests, and no
 * lwIP function is called above thread level: link.c brings frames in on a thread). */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "kernel.h"

extern void KeInitializeSemaphore(void *semaphore, s32 count, s32 limit);
extern s32  KeReleaseSemaphore(void *semaphore, s32 increment, s32 adjustment, u32 wait);
extern void KeQuerySystemTime(s64 *time);
extern void *ExAllocatePoolTypeWithTag(u32 bytes, u32 tag, u32 pool);
extern void ExFreePool(void *p);
extern u32  KeSetPriorityThread(void *thread, u32 priority);
extern u32  KeSetAffinityThread(void *thread, u32 affinity, u32 *previous);
extern void RtlInitializeCriticalSection(void *section);
extern void RtlEnterCriticalSection(void *section);
extern void RtlLeaveCriticalSection(void *section);

#define STATUS_TIMEOUT  0x102u
#define TAG             0x70697773u     /* "swip" */

/* The hardware thread lwIP's own threads run on: not 0 or 1, not 2 (USB), not 4 (the
 * file servers) and not 5 (the driver's worker). To be measured. */
volatile u32 ipstack_cpu = 3;

/* --- semaphores -------------------------------------------------------------- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    KeInitializeSemaphore(sem->object, count, 0x7FFFFFFF);
    sem->valid = 1;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    sem->valid = 0;
}

void sys_sem_signal(sys_sem_t *sem)
{
    KeReleaseSemaphore(sem->object, 0, 1, 0);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    s64 interval = -(s64)timeout * 10000;       /* relative, 100 ns units */

    return KeWaitForSingleObject(sem->object, 0, 0, 0, timeout ? &interval : 0) == STATUS_TIMEOUT
           ? SYS_ARCH_TIMEOUT : 0;
}

/* --- mailboxes --------------------------------------------------------------- */

struct ipstack_mbox {
    sys_sem_t ready;                    /* counts the messages in it */
    KSPIN_LOCK lock;
    u32 size, head, count;
    void *slot[1];
};

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    struct ipstack_mbox *m;

    if (size < 8)
        size = 8;
    m = (struct ipstack_mbox *)ExAllocatePoolTypeWithTag(
            sizeof(*m) + (u32)size * sizeof(void *), TAG, 2);
    if (!m) {
        *mbox = 0;
        return ERR_MEM;
    }
    m->lock = 0;
    m->size = (u32)size;
    m->head = m->count = 0;
    sys_sem_new(&m->ready, 0);
    *mbox = m;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (*mbox)
        ExFreePool(*mbox);
    *mbox = 0;
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    struct ipstack_mbox *m = *mbox;
    u32 irql = KfAcquireSpinLock(&m->lock);

    if (m->count == m->size) {
        KfReleaseSpinLock(&m->lock, irql);
        return ERR_MEM;
    }
    m->slot[(m->head + m->count) % m->size] = msg;
    m->count++;
    KfReleaseSpinLock(&m->lock, irql);
    sys_sem_signal(&m->ready);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    while (sys_mbox_trypost(mbox, msg) != ERR_OK) {
        s64 interval = -10000;          /* full: 1 ms, and again */

        KeDelayExecutionThread(0, 0, &interval);
    }
}

static void *take(struct ipstack_mbox *m)
{
    u32 irql = KfAcquireSpinLock(&m->lock);
    void *msg = m->slot[m->head];

    m->head = (m->head + 1) % m->size;
    m->count--;
    KfReleaseSpinLock(&m->lock, irql);
    return msg;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    struct ipstack_mbox *m = *mbox;
    void *got;

    if (sys_arch_sem_wait(&m->ready, timeout) == SYS_ARCH_TIMEOUT)
        return SYS_ARCH_TIMEOUT;
    got = take(m);
    if (msg)
        *msg = got;
    return 0;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    struct ipstack_mbox *m = *mbox;
    s64 now = 0;
    void *got;

    if (KeWaitForSingleObject(m->ready.object, 0, 0, 0, &now) == STATUS_TIMEOUT)
        return SYS_MBOX_EMPTY;
    got = take(m);
    if (msg)
        *msg = got;
    return 0;
}

/* --- threads ----------------------------------------------------------------- */

#define STARTS 16

static struct {
    lwip_thread_fn fn;
    void *arg;
    u32 priority;
    volatile u32 used;
} starts[STARTS];

static u32 trampoline(void *p)
{
    u32 i = (u32)p, previous;
    lwip_thread_fn fn = starts[i].fn;
    void *arg = starts[i].arg, *self;

    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(self));
    KeSetAffinityThread(self, 1u << ipstack_cpu, &previous);
    KeSetPriorityThread(self, starts[i].priority);
    mem_sync();
    starts[i].used = 0;
    fn(arg);
    return 0;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn fn, void *arg, int stacksize,
                            int priority)
{
    void *thread = 0;
    u32 i;

    (void)name;
    for (i = 0; i < STARTS; i++)
        if (!__sync_lock_test_and_set(&starts[i].used, 1))
            break;
    if (i == STARTS)
        return 0;
    starts[i].fn = fn;
    starts[i].arg = arg;
    starts[i].priority = (u32)priority;
    mem_sync();
    if (ExCreateThread(&thread, (u32)stacksize, 0, 0, (void *)trampoline, (void *)i, 2) < 0) {
        starts[i].used = 0;
        return 0;
    }
    NtClose(thread);
    return thread;
}

/* --- time, protection --------------------------------------------------------- */

u32_t sys_now(void)
{
    s64 t;

    KeQuerySystemTime(&t);
    return (u32_t)((u64)t / 10000u);
}

static u32 section[8] __attribute__((aligned(8)));

void sys_init(void)
{
    RtlInitializeCriticalSection(section);
}

sys_prot_t sys_arch_protect(void)
{
    RtlEnterCriticalSection(section);
    return 0;
}

void sys_arch_unprotect(sys_prot_t was)
{
    (void)was;
    RtlLeaveCriticalSection(section);
}
