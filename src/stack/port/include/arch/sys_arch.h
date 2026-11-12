/* lwIP's operating system layer on the console's kernel; port/sysarch.c. */
#ifndef XE_LWIP_SYS_ARCH_H
#define XE_LWIP_SYS_ARCH_H

/* A kernel semaphore (KSEMAPHORE, 20 bytes) where it stands: lwIP keeps these inside
 * its own objects, which do not move while one is waited on. */
typedef struct {
    unsigned int object[6];
    unsigned int valid;
} __attribute__((aligned(8))) sys_sem_t;

#define sys_sem_valid(s)            ((s) != 0 && (s)->valid)
#define sys_sem_set_invalid(s)      ((s)->valid = 0)

struct ipstack_mbox;
typedef struct ipstack_mbox *sys_mbox_t;

#define sys_mbox_valid(m)           ((m) != 0 && *(m) != 0)
#define sys_mbox_set_invalid(m)     (*(m) = 0)

typedef void *sys_thread_t;
typedef unsigned int sys_prot_t;

#endif
