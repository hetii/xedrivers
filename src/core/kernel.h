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
typedef signed char        s8;
typedef short              s16;
typedef int                s32;
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
    /* Ours, past what the kernel touches: the routine usb_dpc_init was given. */
    PKDEFERRED_ROUTINE Entry;
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
    struct USB_DEV *Dev;                        /* the device it goes to (usb_prepare) */
    void          *Owner;                       /* the driver instance it belongs to */
    u32            Stamp;                       /* time base at submission (prof.h) */
    URB_COMPLETION Done;                        /* +0x40 called at completion (usb.c) */
    URB_COMPLETION Then;                        /* what usb_prepare was asked to call */
} URB;

#if defined(__powerpc__)
_Static_assert(__builtin_offsetof(URB, Done) == 0x40, "usb.c's usb_urb_stub reads it there");
#endif

/* An isochronous request, what UsbdQueueIsochTransfer takes: the common head, the buffer
 * eight bytes earlier than a URB's, then the packet count. The routine gets the request,
 * each packet's status word and its actual length. As read on retail 17559 by
 * usb-audio-x360; +0x19 and +0x1B are the kernel's to fill. */
typedef struct _ISOCH ISOCH;
typedef void (*ISOCH_COMPLETION)(ISOCH *request, u32 *statuses, u16 *lengths);
struct _ISOCH {
    void            *Context;                   /* +0x00, the endpoint */
    ISOCH_COMPLETION Routine;                   /* +0x04 */
    void            *Endpoint;                  /* +0x08 */
    u32              Reserved;                  /* +0x0C */
    void            *Buffer;                    /* +0x10 */
    u32              Length;                    /* +0x14 */
    u8               PacketCount;               /* +0x18 */
    u8               HostController;
    u8               Pad;
    u8               EndpointIndex;
    /* Ours from here on. */
    struct _ISOCH   *Next;                      /* submit queue link */
    struct USB_DEV  *Dev;
    u16             *Lengths;                   /* each packet's length, for the kernel */
    void            *Owner;
    ISOCH_COMPLETION Done;                      /* +0x2C what the driver wants called */
};

#if defined(__powerpc__)
_Static_assert(__builtin_offsetof(struct _ISOCH, Done) == 0x2C, "usb.c's usb_isoch_stub reads it there");
#endif

extern void  UsbdQueueIsochTransfer(void *device, ISOCH *request, u16 *lengths);

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
/* (urb): ordinal 741 -- 17559's 0x800D82E8 takes the controller from urb+18 and
 * returns at once when urb+17 says it is done. */
extern void  UsbdCancelAsyncTransfer(URB *urb);
extern void *UsbdGetRootHubDeviceNode(u32 controller);
extern void *UsbdGetPortDeviceNode(void *hub, u32 port);

/* --- memory, threads, DPCs, locks -------------------------------------------- */

/* MmAllocatePhysicalMemory (185) tail-calls the Ex form with low 0, high ~0 and no
 * alignment: (flags, size, protect). Never from DllMain.
 * xerunner: dllmain-pool-allocation@1 */
extern void *MmAllocatePhysicalMemory(u32 flags, u32 size, u32 protect);
extern void  MmFreePhysicalMemory(u32 flags, void *base);     /* 189 */
extern int   MmIsAddressValid(const void *address);

/* (start, length): dcbst+icbi over the range, then hwsync+isync. Read off ordinal
 * 171 on 17559. Must be called after writing into code, or the old instructions
 * keep running from the instruction cache. */
extern void  KeSweepIcacheRange(void *start, u32 length);

extern int   ExCreateThread(void **handle, u32 stackSize, u32 *threadId,
                            void *apiThreadStartup, void *startRoutine,
                            void *parameter, u32 flags);
extern int   KeDelayExecutionThread(u32 waitMode, u32 alertable, s64 *interval);

/* A dispatcher object is a 16-byte DISPATCHER_HEADER and, for an event, nothing
 * else. Type 1 is a synchronization (auto-reset) event. (event, type, state);
 * (event, increment, wait) -> the previous state; (object, reason, mode,
 * alertable, relative-or-absolute timeout or 0) -> NTSTATUS. Ordinals 112, 157
 * and 176. */
typedef struct { u32 header[4]; } __attribute__((aligned(8))) KEVENT;
extern void  KeInitializeEvent(KEVENT *event, u32 type, u32 state);
extern int   KeSetEvent(KEVENT *event, int increment, u32 wait);
extern u32   KeWaitForSingleObject(void *object, u32 reason, u32 mode, u32 alertable,
                                   s64 *timeout);

extern u32   KeRaiseIrqlToDpcLevel(void);
extern void  KfLowerIrql(u32 irql);
extern u32   KfAcquireSpinLock(KSPIN_LOCK *lock);
extern void  KfReleaseSpinLock(KSPIN_LOCK *lock, u32 oldIrql);
extern void  KeAcquireSpinLockAtRaisedIrql(KSPIN_LOCK *lock);
extern void  KeReleaseSpinLockFromRaisedIrql(KSPIN_LOCK *lock);

extern void  KeInitializeDpc(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context);
extern int   KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2);

/* One setting of the console's own configuration: (category, setting, buffer,
 * size, bytes needed) -> NTSTATUS. Ordinal 16. The console's MAC address is
 * setting 2 of the secured category (net.c, which checks what comes back). */
extern u32   ExGetXConfigSetting(u16 category, u16 setting, void *buffer, u16 size,
                                 u16 *needed);

/* --- the console's own cryptography ------------------------------------------
 * Ordinals 383-386 and 399-402 (HMAC-SHA1 and SHA-1) and 345-346 (AES), which is
 * everything WPA2-Personal needs on the host, so none of it is written here. The
 * states are opaque: the sizes below are generous on purpose, since only the
 * kernel looks inside them. xebuild's XeCrypt headers give the shapes;
 * src/bench/debug checks them against known answers on the console itself.
 *
 * XeCryptHmacSha takes the key and up to three pieces of data, which is what the
 * EAPOL-Key check value, PBKDF2 and the 802.11 key derivation all need. Its key is
 * truncated at 64 bytes rather than hashed, unlike RFC 2104; every key here is 32
 * bytes or less, so that difference never shows. */
typedef struct { u8 opaque[128]; } __attribute__((aligned(8))) XE_SHA_STATE;
typedef struct { u8 opaque[512]; } __attribute__((aligned(8))) XE_HMAC_STATE;
typedef struct { u8 opaque[1024]; } __attribute__((aligned(8))) XE_AES_STATE;

extern void XeCryptShaInit(XE_SHA_STATE *state);
extern void XeCryptShaUpdate(XE_SHA_STATE *state, const u8 *in, u32 length);
extern void XeCryptShaFinal(XE_SHA_STATE *state, u8 *out, u32 out_length);
/* The incremental form, three arguments a call: the one-shot XeCryptHmacSha takes
 * ten, and gcc puts the ninth and tenth where this console does not read them
 * (kit/kcall.S), so it writes no output at all -- checked. */
extern void XeCryptHmacShaInit(XE_HMAC_STATE *state, const u8 *key, u32 key_length);
extern void XeCryptHmacShaUpdate(XE_HMAC_STATE *state, const u8 *in, u32 length);
extern void XeCryptHmacShaFinal(XE_HMAC_STATE *state, u8 *out, u32 out_length);

extern void XeCryptHmacSha(const u8 *key, u32 key_length,
                           const u8 *in1, u32 length1,
                           const u8 *in2, u32 length2,
                           const u8 *in3, u32 length3,
                           u8 *out, u32 out_length);
extern void XeCryptAesKey(XE_AES_STATE *state, const u8 *key);
extern void XeCryptAesEcb(XE_AES_STATE *state, const u8 *in, u8 *out, u32 encrypt);

/* --- files ---------------------------------------------------------------------
 * Object names are ANSI on this kernel. NtCreateFile (210) takes nine arguments, so
 * it is called through kcall9 (kit/kcall.S), which puts the ninth where the console
 * reads it; NtReadFile (240) takes eight, all in registers: (handle, event, apc
 * routine, apc context, io status, buffer, length, byte offset or 0). NtClose 207.
 * xerunner examples/plugins/plugin-probe.c. */
typedef struct { u16 Length, MaximumLength; const char *Buffer; } ANSI_STRING;
typedef struct { void *RootDirectory; ANSI_STRING *ObjectName; u32 Attributes; }
    OBJECT_ATTRIBUTES;
typedef struct { u32 Status; u32 Information; } IO_STATUS_BLOCK;
extern u32   NtCreateFile(void **handle, u32 access, OBJECT_ATTRIBUTES *attributes,
                          IO_STATUS_BLOCK *io, s64 *allocation, u32 file_attributes,
                          u32 share, u32 disposition, u32 options);
extern u32   NtReadFile(void *handle, void *event, void *apc, void *apc_context,
                        IO_STATUS_BLOCK *io, void *buffer, u32 length, s64 *offset);
extern u32   NtWriteFile(void *handle, void *event, void *apc, void *apc_context,
                         IO_STATUS_BLOCK *io, const void *buffer, u32 length, s64 *offset);
extern u32   NtClose(void *handle);
extern u32   kcall9(u32 *args);

/* --- the wired NIC interface xnet calls -------------------------------------
 * Imported so the loader hands us the kernel's own addresses: our hooks call these
 * and xam's thunks are re-pointed at the hooks. Shapes from the 17559 disassembly
 * and CUsbEnet's recovered prototypes. */
extern void  NicAttach(void *attachInfo, void **userOut);           /* 514 */
extern void  NicDetach(void *user);                                 /* 515 */
extern void  NicXmit(void *user, u32 queue, void *frame, u32 length,
                     void *cookie);                                 /* 516 */
extern u32   NicGetLinkState(u32 interfaces);                       /* 520 */
/* A wireless device (kind 2) for the kernel's NIC layer: its first two words a
 * list entry, +0x08 its type, +0x0C its services table. 0 when taken. */
extern s32   NicRegisterDevice(u32 kind, void *device);              /* 739 */
extern void  NicUnregisterDevice(u32 kind, void *device);            /* 771 */

#endif /* XE_KERNEL_H */
