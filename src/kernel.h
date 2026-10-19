/* The console kernel's types and the exports this driver calls.
 *
 * No ordinal appears here: kit/plugin.py reads the undefined symbols out of the
 * objects and resolves each against kit/ordinals-xboxkrnl-21256.0.txt (and xam's
 * table for the TCP server), so a misspelt name
 * fails the build instead of becoming a wrong ordinal. Every shape below was read
 * off the running 17559 kernel (dumped over XBDM) or taken from a claim in the
 * xerunner base; where it came from is noted at the declaration.
 *
 * Calling convention: arguments in r3..r10, result in r3, the same in gcc's SysV
 * ABI and the console's for up to eight integer arguments. r13 is the processor
 * control region on the console; gcc never touches it with -msdata=none, which is
 * what this tree builds with (see the Makefile).
 */

#ifndef XE_KERNEL_H
#define XE_KERNEL_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef short              s16;
typedef long long          s64;

#define STATUS_SUCCESS       0x00000000u
#define STATUS_UNSUCCESSFUL  0xC0000001u
#define NT_SUCCESS(s)        ((int)(s) >= 0)

#define PASSIVE_LEVEL  0
#define DISPATCH_LEVEL 2

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

typedef u32 KSPIN_LOCK;     /* one word; KfAcquireSpinLock lwarx/stwcx's r13 into it */

/* KeInitializeDpc (ordinal 111) on 17559:
 *   +0x00 u16 Type = 0x13   +0x02 u8 InsertedNumber   +0x03 u8 TargetNumber
 *   +0x04 LIST_ENTRY        +0x0C routine             +0x10 context
 *   +0x14 arg1              +0x18 arg2                 size 0x1C
 * KeInsertQueueDpc reads TargetNumber: 0 = this processor, N = processor N-1. */
struct _KDPC;
typedef void (*PKDEFERRED_ROUTINE)(struct _KDPC *dpc, void *context, void *arg1,
                                   void *arg2);
typedef struct _KDPC {
    u16                Type;
    u8                 InsertedNumber;
    u8                 TargetNumber;
    LIST_ENTRY         DpcListEntry;
    PKDEFERRED_ROUTINE DeferredRoutine;
    void              *DeferredContext;
    void              *SystemArgument1;
    void              *SystemArgument2;
} KDPC;

/* Read off KeRaiseIrqlToDpcLevel and KeInsertQueueDpc: IRQL is the byte at PCR+0x18,
 * the current hardware thread the byte at PCR+0x10C. The host build of tests/ gets
 * stand-ins: those translation units never reach the console's PCR. */
#if defined(__powerpc__)
static inline u32 ke_irql(void)
{
    u32 v;
    __asm__ volatile("lbz %0, 0x18(13)" : "=r"(v));
    return v;
}

static inline u32 ke_processor(void)
{
    u32 v;
    __asm__ volatile("lbz %0, 0x10c(13)" : "=r"(v));
    return v;
}

static inline void mem_sync(void)
{
    __asm__ volatile("sync" ::: "memory");
}
#else
static inline u32 ke_irql(void) { return 0; }
static inline u32 ke_processor(void) { return 0; }
static inline void mem_sync(void) { __sync_synchronize(); }
#endif

/* Frame copy into transfer buffers, a word at a time when source and destination
 * share their alignment (Ethernet frames usually sit at 2 mod 4 on both sides, so
 * two bytes go first). The byte loop this replaces cost about 60 us a frame and
 * was what held transmit at 15.9k frames a second in ECM and NCM alike. */
typedef u32 __attribute__((may_alias)) u32_alias;

static inline void xe_copy(u8 *dst, const u8 *src, u32 n)
{
    if ((((unsigned long)dst ^ (unsigned long)src) & 3) == 0) {
        u32_alias *d;
        const u32_alias *s;

        while (n && ((unsigned long)dst & 3)) {
            *dst++ = *src++;
            n--;
        }
        d = (u32_alias *)dst;
        s = (const u32_alias *)src;
        while (n >= 16) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
            d += 4;
            s += 4;
            n -= 16;
        }
        while (n >= 4) {
            *d++ = *s++;
            n -= 4;
        }
        dst = (u8 *)d;
        src = (const u8 *)s;
    }
    while (n--)
        *dst++ = *src++;
}

/* --- USB ----------------------------------------------------------------------
 * xerunner: usb-driver-object-f1@1 -- the 28-byte driver object. */
typedef struct _USBD_DRIVER_OBJECT {
    LIST_ENTRY ListEntry;                       /* both words zero before register */
    u32        Private;
    int      (*AddDevice)(void *device);
    int      (*RemoveDevice)(void *device);
    u32      (*Match)(const u8 *interfaceDescriptor, const u8 *deviceDescriptor,
                      struct _USBD_DRIVER_OBJECT *self);
    void     (*Notify)(void *device, u32 reason, void *context);
} USBD_DRIVER_OBJECT;

/* The request the kernel is handed. +0x00..+0x13 is the async header (completion at
 * +0x04, endpoint at +0x08, routing bytes the kernel writes at +0x10..+0x13); a
 * transfer adds buffer, length and the count actually moved, a control transfer the
 * setup packet in USB byte order. The completion routine is called with the request
 * and the NTSTATUS. xerunner: usb-transfers-f2@1, xerunner: usb-transfers-f4@1,
 * xerunner: usb-transfers-f9@1; BytesTransferred at +0x1C and the second argument
 * as in CUsbEnet's recovered USBD_TRANSFER_REQUEST. */
struct _URB;
typedef void (*URB_COMPLETION)(struct _URB *urb, u32 status);
typedef struct _URB {
    void          *Context;                     /* +0x00 */
    URB_COMPLETION Routine;                     /* +0x04 */
    void          *Endpoint;                    /* +0x08 */
    void          *InternalNext;                /* +0x0C */
    u8             Flags;                       /* +0x10, cleared before queueing */
    u8             CancelPending;
    u8             HostController;
    u8             DeviceNode;
    void          *Buffer;                      /* +0x14 */
    u32            Length;                      /* +0x18 */
    u32            Actual;                      /* +0x1C */
    u8             Setup[8];                    /* +0x20 */
    /* Ours from here on; the kernel never reads past +0x28. */
    struct _URB   *Next;                        /* submit queue link */
    u32            Tag;
    u32            Busy;
    u32            Spare;
} URB;

extern int   UsbdRegisterDriverObject(USBD_DRIVER_OBJECT *driver);
extern void  UsbdAddDeviceComplete(void *device, u32 status);
extern void  UsbdRemoveDeviceComplete(void *device);
extern u8   *UsbdGetDeviceDescriptor(void *device);
extern int   UsbdOpenDefaultEndpoint(void *device, void **endpointOut);
/* (device, type, bEndpointAddress, wMaxPacketSize, bInterval raw, handle out).
 * xerunner: usb-transfers-f7@1 */
extern int   UsbdOpenEndpoint(void *device, u32 type, u32 address, u32 maxPacket,
                              u32 interval, void **endpointOut);
/* Returns nothing usable. xerunner: usbd-queue-async-return@1 */
extern void  UsbdQueueAsyncTransfer(void *device, URB *urb);
/* (device, request); the endpoint is read from request+0x08.
 * xerunner: usb-transfers-f10@1 */
extern void  UsbdQueueCloseDefaultEndpoint(void *device, URB *urb);
extern void  UsbdQueueCloseEndpoint(void *device, URB *urb);
extern int   UsbdResetDevice(void *device);
extern void *UsbdGetRootHubDeviceNode(u32 controller);
extern void *UsbdGetPortDeviceNode(void *hub, u32 port);

/* --- memory, threads, DPCs, locks -------------------------------------------- */

/* MmAllocatePhysicalMemory (185) tail-calls the Ex form with low 0, high ~0 and no
 * alignment: (flags, size, protect). Never from DllMain.
 * xerunner: dllmain-pool-allocation@1 */
extern void *MmAllocatePhysicalMemory(u32 flags, u32 size, u32 protect);
extern int   MmIsAddressValid(const void *address);

/* (start, length): dcbst+icbi over the range, then hwsync+isync. Read off ordinal
 * 171 on 17559. Must be called after writing into code, or the old instructions
 * keep running from the instruction cache. */
extern void  KeSweepIcacheRange(void *start, u32 length);

extern int   ExCreateThread(void **handle, u32 stackSize, u32 *threadId,
                            void *apiThreadStartup, void *startRoutine,
                            void *parameter, u32 flags);
extern int   KeDelayExecutionThread(u32 waitMode, u32 alertable, s64 *interval);

extern u32   KeRaiseIrqlToDpcLevel(void);
extern void  KfLowerIrql(u32 irql);
extern u32   KfAcquireSpinLock(KSPIN_LOCK *lock);
extern void  KfReleaseSpinLock(KSPIN_LOCK *lock, u32 oldIrql);
extern void  KeAcquireSpinLockAtRaisedIrql(KSPIN_LOCK *lock);
extern void  KeReleaseSpinLockFromRaisedIrql(KSPIN_LOCK *lock);

extern void  KeInitializeDpc(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context);
extern int   KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2);

/* --- the wired NIC interface xnet calls -------------------------------------
 * Imported so the loader hands us the kernel's own addresses: our hooks call these
 * and xam's thunks are re-pointed at the hooks. Shapes from the 17559 disassembly
 * and CUsbEnet's recovered prototypes. */
extern void  NicAttach(void *attachInfo, void **userOut);           /* 514 */
extern void  NicDetach(void *user);                                 /* 515 */
extern void  NicXmit(void *user, u32 queue, void *frame, u32 length,
                     void *cookie);                                 /* 516 */
extern u32   NicGetLinkState(u32 interfaces);                       /* 520 */

#endif /* XE_KERNEL_H */
