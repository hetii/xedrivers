/* The kernel's USB driver interface, for up to USB_MAX_DEVICES devices at once.
 *
 *   Match         for every interface of every device that enumerates, ours first
 *                 xerunner: usb-driver-object-f4@1, xerunner: usb-driver-object-f5@1
 *   AddDevice     open the default endpoint, complete synchronously
 *                 xerunner: usb-driver-object-f7@1, xerunner: usb-driver-object-f16@1
 *   RemoveDevice  close every endpoint, last the default one, then complete
 *                 xerunner: usb-driver-object-f18@1, xerunner: usb-transfers-f10@1
 */

#include "usb.h"
#include "prof.h"

USB_DEV usb_devs[USB_MAX_DEVICES];
volatile USB_STATS usb_stats;

static USBD_DRIVER_OBJECT driver_object;
static const USB_CLIENT *client;

/* Requests waiting for the USB processor. */
static KSPIN_LOCK submit_lock;
static URB *submit_head;
static URB *submit_tail;
static KDPC submit_dpc;

/* Requests to cancel, for the USB processor too (usb_cancel). */
#define CANCEL_MAX 16
static URB *cancel_list[CANCEL_MAX];
static volatile u32 cancel_count;
static KDPC cancel_dpc;


static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

/* The wake event (see usb_wake below). */
static KEVENT wake_event;
static volatile u32 wake_ready;

#define SPIN_TB 50000u                  /* 1 ms of the 50 MHz time base */

/* Waits on the USB processor, during a bring-up (main.c): counted on the time base, the
 * thread running all along. Once the dashboard has dimmed the screen, a thread waiting there
 * is woken neither by its timeout nor by its event (main.c, WORKER_PROCESSOR), so a device
 * plugged in then stopped in its first wait until a pad moved (2026-11-09, an AIC8800). A
 * thread that never waits is not stopped. usb_wake ends such a wait early, as it does the
 * event's. */
static volatile u32 spin_waits, spun_woken;

void usb_spin_waits(u32 on)
{
    spin_waits = on;
    if (on)
        usb_stats.spin_bring_ups++;
}

static u32 timebase(void)
{
#if defined(__powerpc__)
    u32 t;

    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}

static void spin(u32 ms)
{
    u32 start = timebase();

    spun_woken = 0;
    while (!spun_woken && timebase() - start < ms * SPIN_TB) {
#if !defined(__powerpc__)
        break;
#endif
    }
}

void usb_sleep(u32 ms)
{
    s64 interval = -((s64)ms * 10000);          /* relative, 100 ns units */

    if (spin_waits) {
        spin(ms);
        return;
    }
    KeDelayExecutionThread(0, 0, &interval);
}

/* The worker's alarm clock. A timed wait alone is not enough: once the dashboard
 * dims the screen after ten idle minutes, a thread waiting only on a timeout here
 * is not woken again until somebody touches a pad (measured: the worker stopped at
 * the same tick every time, and went on as soon as the pad moved). So the worker
 * and the control transfers it waits for also wait on this event, which whatever
 * they wait for sets -- a device arriving or leaving, a control transfer done. */

void usb_wake(void)
{
    spun_woken = 1;
    if (wake_ready)
        KeSetEvent(&wake_event, 0, 0);
}

void usb_wait(u32 ms)
{
    s64 timeout = -((s64)ms * 10000);

    if (!wake_ready || spin_waits) {
        usb_sleep(ms);
        return;
    }
    KeWaitForSingleObject(&wake_event, 0, 0, 0, &timeout);
}

/* Every way the kernel calls into us at DISPATCH_LEVEL goes through one of these. A DPC runs
 * on whatever thread the processor was in, and only r0-r12 of it are saved on the way in: r14-r31
 * are that thread's own, live, 64 bits wide. gcc in -m32 saves what it uses with stw and lwz,
 * so the upper half of each comes back zero. A game holding a 64-bit value or a sign-extended
 * pointer across the interrupt then runs on with it broken, crashing anywhere. The kernel's DPC
 * loop saves r28-r31 itself, which is why a routine that stayed within those was fine and the same
 * work using r26 and r27 was not (0 of 8 boots against 12 of 12, 2026-11-06, Aurora).
 * Costs 18 stores and 18 loads a completion. */
#if defined(__powerpc__)
USB_STUB("usb_urb_stub", "lwz 12, 0x40(3)");    /* (urb, status): urb->Done */
USB_STUB("usb_isoch_stub", "lwz 12, 0x2C(3)");  /* (request, statuses, lengths): request->Done */
USB_STUB("usb_dpc_stub", "lwz 12, 0x1C(3)");    /* (dpc, context, ...): dpc->Entry */

void usb_urb_stub(URB *urb, u32 status);
void usb_isoch_stub(ISOCH *request, u32 *statuses, u16 *lengths);
void usb_dpc_stub(KDPC *dpc, void *context, void *a1, void *a2);
#define URB_ENTRY usb_urb_stub

void usb_dpc_init(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context)
{
    dpc->Entry = routine;
    KeInitializeDpc(dpc, usb_dpc_stub, context);
}
#else
#define URB_ENTRY urb->Done
#endif

/* The kernel follows the Windows PowerPC convention: a callee may store its arguments in
 * the caller's frame, at SP+0x10 and up (kit/kcall.S), and gcc keeps saved registers there.
 * The OHCI side of UsbdQueueAsyncTransfer does so (17559, 0x800dc698: stw r5,276(r1) in a
 * frame of 240), writing the controller's address over what its caller had saved: with
 * submit_dpc_routine's frame that was r29, and the kernel's DPC loop returned through it,
 * into the controller object. The whole console stopped. The EHCI side stores nothing, and
 * nothing else from 0x800d8000 to 0x800de000 does; so only this call gets a frame of its own.
 * Seen on the UART by bench_trace, 2026-11-06. */
#if defined(__powerpc__)
__asm__(".section .text\n"
        ".globl usb_framed_call\n"
        "usb_framed_call:\n"
        "    mflr    0\n"
        "    stw     0, 4(1)\n"
        "    stwu    1, -128(1)\n"
        "    mtctr   3\n"
        "    mr      3, 4\n"
        "    mr      4, 5\n"
        "    mr      5, 6\n"
        "    mr      6, 7\n"
        "    mr      7, 8\n"
        "    mr      8, 9\n"
        "    mr      9, 10\n"
        "    bctrl\n"
        "    addi    1, 1, 128\n"
        "    lwz     0, 4(1)\n"
        "    mtlr    0\n"
        "    blr\n");
#endif

static void submit_now(URB *urb)
{
    if (!urb->Dev || !urb->Dev->device) {
        urb->Busy = 0;
        return;
    }
    mem_sync();                     /* buffer contents visible before the HC looks */
    {
        PROF_START(t);
#if defined(__powerpc__)
        if (USB_ON_COMPANION(urb->Dev))
            usb_framed_call((void *)UsbdQueueAsyncTransfer, urb->Dev->device, urb);
        else
#endif
            UsbdQueueAsyncTransfer(urb->Dev->device, urb);
        PROF_END(submit, t, urb->Length);
    }
}

static ISOCH *isoch_head, *isoch_tail;

static void isoch_now(ISOCH *request)
{
    USB_DEV *dev = request->Dev;

    if (dev->active && dev->device)
        UsbdQueueIsochTransfer(dev->device, request, request->Lengths);
}

void usb_isoch(USB_DEV *dev, ISOCH *request, u16 *lengths)
{
    u32 irql;

    request->Dev = dev;
    request->Lengths = lengths;
#if defined(__powerpc__)
    request->Routine = usb_isoch_stub;
#else
    request->Routine = request->Done;
#endif
    if (ke_irql() >= DISPATCH_LEVEL && ke_processor() == USB_PROCESSOR) {
        isoch_now(request);
        return;
    }
    request->Next = 0;
    irql = KfAcquireSpinLock(&submit_lock);
    if (isoch_tail)
        isoch_tail->Next = request;
    else
        isoch_head = request;
    isoch_tail = request;
    KfReleaseSpinLock(&submit_lock, irql);
    KeInsertQueueDpc(&submit_dpc, 0, 0);
}

static void submit_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    URB *list;
    ISOCH *isoch;

    (void)dpc;
    (void)context;
    (void)a1;
    (void)a2;

    usb_stats.dpc_runs++;
    KeAcquireSpinLockAtRaisedIrql(&submit_lock);
    list = submit_head;
    submit_head = 0;
    submit_tail = 0;
    isoch = isoch_head;
    isoch_head = 0;
    isoch_tail = 0;
    KeReleaseSpinLockFromRaisedIrql(&submit_lock);

    while (isoch) {
        ISOCH *next = isoch->Next;

        isoch->Next = 0;
        isoch_now(isoch);
        isoch = next;
    }

    while (list) {
        URB *next = list->Next;
        list->Next = 0;
        submit_now(list);
        list = next;
    }
}

void usb_submit(URB *urb)
{
    u32 irql;

    urb->Busy = 1;
    if (ke_irql() >= DISPATCH_LEVEL && ke_processor() == USB_PROCESSOR) {
        usb_stats.submit_direct++;
        submit_now(urb);
        return;
    }

    usb_stats.submit_dpc++;
    urb->Next = 0;
    irql = KfAcquireSpinLock(&submit_lock);
    if (submit_tail)
        submit_tail->Next = urb;
    else
        submit_head = urb;
    submit_tail = urb;
    KfReleaseSpinLock(&submit_lock, irql);
    KeInsertQueueDpc(&submit_dpc, 0, 0);
}

static void cancel_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    URB *list[CANCEL_MAX];
    u32 n, i;

    (void)dpc;
    (void)context;
    (void)a1;
    (void)a2;
    KeAcquireSpinLockAtRaisedIrql(&submit_lock);
    n = cancel_count;
    for (i = 0; i < n; i++)
        list[i] = cancel_list[i];
    cancel_count = 0;
    KeReleaseSpinLockFromRaisedIrql(&submit_lock);
    for (i = 0; i < n; i++)
        if (list[i]->Busy) {
            usb_stats.cancels++;
            UsbdCancelAsyncTransfer(list[i]);
        }
}

/* Take back a request the controller holds: it completes, cancelled, as any other
 * would. Run on the USB processor like a submit. Any IRQL. */
void usb_cancel(URB *urb)
{
    u32 irql = KfAcquireSpinLock(&submit_lock);

    if (cancel_count < CANCEL_MAX)
        cancel_list[cancel_count++] = urb;
    KfReleaseSpinLock(&submit_lock, irql);
    KeInsertQueueDpc(&cancel_dpc, 0, 0);
}

void usb_rx_watch_init(USB_RX_WATCH *w, URB *urbs, u32 count, u32 kick_ticks,
                       u32 reset_ticks, volatile u32 *kicks, volatile u32 *kick_ok)
{
    w->urbs = urbs;
    w->count = count;
    w->kick_ticks = kick_ticks;
    w->reset_ticks = reset_ticks;
    w->seen = 0;
    w->quiet = 0;
    w->kicked = 0;
    w->kicks = kicks;
    w->kick_ok = kick_ok;
}

u32 usb_rx_watch(USB_RX_WATCH *w, u32 watched, u32 progress)
{
    u32 i;

    if (!watched || progress != w->seen) {
        if (w->kicked && watched && w->kick_ok)
            (*w->kick_ok)++;
        w->kicked = 0;
        w->seen = progress;
        w->quiet = 0;
        return 0;
    }
    if (++w->quiet >= w->kick_ticks && w->kick_ticks && !w->kicked) {
        if (w->kicks)
            (*w->kicks)++;
        w->kicked = 1;
        for (i = 0; i < w->count; i++)
            usb_cancel(&w->urbs[i]);
        return 0;
    }
    if (w->quiet < w->reset_ticks)
        return 0;
    w->quiet = 0;
    return 1;
}

void usb_prepare(URB *urb, USB_DEV *dev, void *endpoint, URB_COMPLETION done,
                 void *buffer, u32 length, const u8 *setup)
{
    u32 i;
    u8 *raw = (u8 *)urb;

    /* The header the kernel owns starts zeroed; the drivers that build these
     * memset the enclosing structure. Our own fields after +0x28 are kept. */
    for (i = 0; i < 0x28; i++)
        raw[i] = 0;
    urb->Context = urb;
    urb->Done = done;
    urb->Routine = URB_ENTRY;
    urb->Endpoint = endpoint;
    /* OHCI: a short IN is no error (the kernel's queue reads +0x10: buffer rounding). */
    if (USB_ON_COMPANION(dev))
        urb->Flags = 1;
    urb->Dev = dev;
    urb->Buffer = length ? buffer : 0;
    urb->Length = length;
    if (setup) {
        for (i = 0; i < 8; i++)
            urb->Setup[i] = setup[i];
    }
}

/* --- synchronous control transfers, for the bring-up thread ------------------ */

/* A synchronous transfer is mostly done in a fraction of a millisecond: watching
 * for that here first means nobody waits on wake_event yet when the completion sets
 * it, so the USB processor does not have to send this one a request to run again.
 * Two processors sending each other requests for every transfer of a bring-up is
 * what both stopped in (see main.c). The timebase runs at 50 MHz. */

void usb_spin(volatile u32 *done)
{
#if defined(__powerpc__)
    u32 start, now;

    __asm__ volatile("mftb %0" : "=r"(start));
    do {
        if (*done)
            return;
        __asm__ volatile("mftb %0" : "=r"(now));
    } while (now - start < SPIN_TB);
#else
    (void)done;
#endif
}

u32 usb_wait_idle(URB *urb, u32 timeout_ms)
{
    u32 waited;

    for (waited = 0; urb->Busy; waited++) {
        if (waited >= timeout_ms)
            return 0;
        usb_wait(1);
    }
    return 1;
}

static void control_complete(URB *urb, u32 status)
{
    USB_DEV *dev = urb->Dev;

    dev->control_status = status;
    urb->Busy = 0;
    mem_sync();
    dev->control_done = 1;
    usb_wake();
}

u32 usb_control(USB_DEV *dev, const u8 setup[8], void *buffer, u32 length,
                u32 *actual, u32 timeout_ms)
{
    u32 waited;

    if (!dev->active || !dev->control)
        return STATUS_UNSUCCESSFUL;
    if (!usb_wait_idle(&dev->control_urb, timeout_ms)) {
        usb_stats.control_late++;
        return STATUS_UNSUCCESSFUL;
    }

    dev->control_done = 0;
    usb_prepare(&dev->control_urb, dev, dev->control, control_complete, buffer, length,
                setup);
    usb_submit(&dev->control_urb);
    usb_spin(&dev->control_done);

    for (waited = 0; !dev->control_done; waited++) {
        if (waited >= timeout_ms)
            return STATUS_UNSUCCESSFUL;
        usb_wait(1);
    }
    if (actual)
        *actual = dev->control_urb.Actual;
    return dev->control_status;
}

/* --- endpoints ---------------------------------------------------------------- */

/* The NAK count reload of a high-speed bulk endpoint (RL, bits 31:28 of its
 * queue head's second dword). The kernel leaves it 0, so the host controller
 * retries an IN that answers NAK on every pass of the async schedule, and the
 * bus time goes to polling the receive endpoint while the transmit one has data.
 * With 1 it skips that queue head after one NAK until the next microframe.
 * The endpoint handle UsbdOpenEndpoint returns is the kernel's EHCI QH (xboxkrnl
 * 21256 UsbdEhciInitializeEndpoint writes dword1 at +4 little-endian, and 17559
 * the same: 0x02002401 for an RTL8822BU's 0x84); it is changed only when that
 * word holds this endpoint's number, maximum packet and high speed, so an OHCI
 * ED (a full-speed device) is never touched. Measured on the RTL8822BU, 5 GHz,
 * A/B three times each (2026-11-03): one stream out 161-165 -> 173-175 Mbit/s,
 * eight 168-169 -> 179 (22.4 MB/s); in unchanged (208-210); RL 2 174, 3 172,
 * 4, 8, 15 no gain. */
#define NAK_RELOAD          1u

static void nak_reload(void *endpoint, u32 address, u32 max_packet)
{
    volatile u32 *w = (volatile u32 *)((u8 *)endpoint + 4);
    u32 v;

#if defined(__powerpc__)
    __asm__ volatile("lwbrx %0, 0, %1" : "=r"(v) : "r"(w) : "memory");
#else
    v = *w;
#endif
    if (((v >> 12) & 3) != 2 || ((v >> 8) & 15) != (address & 15) ||
        ((v >> 16) & 0x7FF) != (max_packet & 0x7FF) || !(v & 0x7F)) {
        usb_stats.nak_reload_skipped++;
        return;
    }
    v = (v & 0x0FFFFFFFu) | NAK_RELOAD << 28;
#if defined(__powerpc__)
    __asm__ volatile("stwbrx %0, 0, %1; eieio" :: "r"(v), "r"(w) : "memory");
#else
    *w = v;
#endif
    usb_stats.nak_reload_set++;
}

void *usb_open(USB_DEV *dev, u32 type, u32 address, u32 max_packet, u32 interval)
{
    void *endpoint = 0;

    if (!dev->active || dev->ep_count >= sizeof(dev->ep) / sizeof(dev->ep[0]))
        return 0;
    usb_stats.last_open_status = (u32)UsbdOpenEndpoint(dev->device, type, address,
                                                        max_packet, interval,
                                                        &endpoint);
    if (!endpoint)
        return 0;
    dev->ep[dev->ep_count++] = endpoint;
    if (type == USB_TYPE_BULK)
        nak_reload(endpoint, address, max_packet);
    return endpoint;
}

/* Each pass closes one endpoint and clears it BEFORE queueing, so the re-entry
 * already sees the next step; a pass that still saw the endpoint it just closed
 * would queue it again forever. */
static void close_complete(URB *urb, u32 status)
{
    USB_DEV *dev = urb->Dev;
    void *endpoint;

    (void)status;
    usb_stats.close_passes++;

    while (dev->ep_count > 0) {
        endpoint = dev->ep[dev->ep_count - 1];
        dev->ep[dev->ep_count - 1] = 0;
        dev->ep_count--;
        if (endpoint) {
            usb_prepare(urb, dev, endpoint, close_complete, 0, 0, 0);
            UsbdQueueCloseEndpoint(dev->device, urb);
            return;
        }
    }
    if (dev->control) {
        endpoint = dev->control;
        dev->control = 0;
        usb_prepare(urb, dev, endpoint, close_complete, 0, 0, 0);
        UsbdQueueCloseDefaultEndpoint(dev->device, urb);
        return;
    }

    endpoint = dev->device;
    dev->device = 0;
    mem_sync();
    UsbdRemoveDeviceComplete(endpoint);
}

/* --- the driver object -------------------------------------------------------- */

static int ours(const u8 *descriptor)
{
    return descriptor && client && client->match(0, descriptor);
}

USB_ENTERED u32 usb_on_match(const u8 *interfaceDescriptor, const u8 *deviceDescriptor,
                 USBD_DRIVER_OBJECT *self)
{
    (void)self;
    usb_stats.match_calls++;
    return client && client->match(interfaceDescriptor, deviceDescriptor) ? 1u : 0u;
}

static USB_DEV *slot_of(void *device)
{
    u32 i;

    for (i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devs[i].device == device)
            return &usb_devs[i];
    return 0;
}

USB_ENTERED int usb_on_add(void *device)
{
    USB_DEV *dev;
    int status;

    usb_stats.add_calls++;
    dev = slot_of(0);                           /* a free slot */
    if (!dev || !ours(UsbdGetDeviceDescriptor(device))) {
        if (!dev)
            usb_stats.no_slot++;
        UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return (int)STATUS_UNSUCCESSFUL;
    }

    dev->device = device;
    dev->ep_count = 0;
    dev->control = 0;
    /* The device handed to AddDevice is its node (the node tables show the same
     * address), whose byte 8 says controller and port. */
    dev->where = ((const u8 *)device)[8];
    status = UsbdOpenDefaultEndpoint(device, &dev->control);
    if (!NT_SUCCESS(status) || !dev->control) {
        dev->control = 0;
        dev->device = 0;
        UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return (int)STATUS_UNSUCCESSFUL;
    }

    /* Before the generation moves: the worker brings the device up as soon as it
     * sees it, and must find the driver already chosen. */
    if (client->attached)
        client->attached(dev);
    dev->generation++;
    mem_sync();
    dev->active = 1;
    UsbdAddDeviceComplete(device, STATUS_SUCCESS);
    usb_wake();
    return 0;
}

USB_ENTERED int usb_on_remove(void *device)
{
    USB_DEV *dev = device ? slot_of(device) : 0;

    usb_stats.remove_calls++;
    if (!dev) {
        UsbdRemoveDeviceComplete(device);
        return 0;
    }

    dev->active = 0;
    mem_sync();
    if (client->detached)
        client->detached(dev);
    usb_wake();
    dev->close_urb.Dev = dev;
    close_complete(&dev->close_urb, 0);
    return 0;
}

/* The driver object's three, which the kernel calls while it enumerates, in a DPC on whatever
 * thread was running: through the stubs that keep that thread's registers (USB_STUB). */
#if defined(__powerpc__)
USB_ENTRY(usb_match_entry, usb_on_match);
USB_ENTRY(usb_add_entry, usb_on_add);
USB_ENTRY(usb_remove_entry, usb_on_remove);
u32 usb_match_entry(const u8 *interfaceDescriptor, const u8 *deviceDescriptor, USBD_DRIVER_OBJECT *self);
int usb_add_entry(void *device);
int usb_remove_entry(void *device);
#endif

int usb_register(const USB_CLIENT *who)
{
    u32 i;

    for (i = 0; i < USB_MAX_DEVICES; i++)
        usb_devs[i].index = i;
    client = who;
    KeInitializeEvent(&wake_event, 1, 0);
    mem_sync();
    wake_ready = 1;
    usb_dpc_init(&submit_dpc, submit_dpc_routine, 0);
    submit_dpc.TargetNumber = USB_PROCESSOR + 1;
    usb_dpc_init(&cancel_dpc, cancel_dpc_routine, 0);
    cancel_dpc.TargetNumber = USB_PROCESSOR + 1;

    /* xerunner: usb-driver-object-f2@1 -- both list words zero or a bug check. */
    driver_object.ListEntry.Flink = 0;
    driver_object.ListEntry.Blink = 0;
    driver_object.Private = 0;
#if defined(__powerpc__)
    driver_object.AddDevice = usb_add_entry;
    driver_object.RemoveDevice = usb_remove_entry;
    driver_object.Match = usb_match_entry;
#else
    driver_object.AddDevice = usb_on_add;
    driver_object.RemoveDevice = usb_on_remove;
    driver_object.Match = usb_on_match;
#endif
    driver_object.Notify = 0;
    return UsbdRegisterDriverObject(&driver_object);
}

/* Port lookups assert DISPATCH_LEVEL, so the walk runs raised; it is a handful of
 * table reads. Four root hubs, eight ports each covers the console's controllers
 * and the lookup does not range-check its index. The reset itself runs back at
 * PASSIVE_LEVEL, as the debug channel in xdrivers called it.
 *
 * What a node is, read off the 17559 code and a live dump: 16 bytes in the
 * controller's node table; words 0 and 1 are zero until a driver has the device;
 * byte 8 holds the controller (bits 0-1) and the port (bits 2-4); bytes 13, 14
 * and 15 link parent, first child and next sibling. UsbdGetPortDeviceNode (881)
 * follows those links for any node, so the same call walks below a hub.
 *
 * UsbdGetDeviceDescriptor (759) does NOT answer for a node: it returns one of four
 * shared descriptor slots chosen by bits of byte 11. With the card on the front
 * port (EHCI 1, port 0) and the Orange Pi on port 2, both chose slot 1 and the card
 * read as a second Orange Pi; on the back port the card happened to have a slot of
 * its own. So every node no driver has is reset once -- the kernel then
 * enumerates it again and hands Match its real descriptor, and whatever is not
 * ours stays unclaimed as it was; with several devices to own, all of them come
 * back this way. Nodes with children (hubs, composite devices) are left alone. */
#define HUB_DEPTH     2
#define UNBOUND_MAX   8

typedef struct {
    u32 driver[2];          /* zero until a driver has the device */
    u8  where;              /* controller bits 0-1, port bits 2-4 */
    u8  other[4];
    u8  parent;
    u8  child;              /* first child, 0xFF for none */
    u8  sibling;
} USB_NODE;

/* Every node the last walk saw, for xectl.py: controller << 24 | depth << 16 |
 * port << 8 | bound, then the descriptor slot's VID << 16 | PID (which, see
 * above, may belong to another device). */
volatile u32 usb_seen[32];
static u32 seen_count;

static void *unbound[UNBOUND_MAX];
static u32 unbound_count;
static void *was_reset[UNBOUND_MAX];
static u32 was_reset_count;

static void find_below(void *hub, u32 depth)
{
    u32 port;

    for (port = 0; port < 8; port++) {
        USB_NODE *node = (USB_NODE *)UsbdGetPortDeviceNode(hub, port);
        const u8 *dd;
        u32 bound;

        if (!node)
            continue;
        dd = UsbdGetDeviceDescriptor(node);
        bound = node->driver[0] || node->driver[1];
        if (seen_count < 16) {
            usb_seen[2 * seen_count] = (u32)(node->where & 3) << 24 | depth << 16 |
                                       ((node->where >> 2) & 7u) << 8 | bound;
            usb_seen[2 * seen_count + 1] = dd ? (u32)le16(dd + 8) << 16 |
                                                le16(dd + 10) : 0;
            seen_count++;
        }
        /* A node with children is a hub or a composite device whose functions
         * are bound below it (the internal 045e:02aa is one): never reset. */
        if (!bound && node->child == 0xFF && unbound_count < UNBOUND_MAX)
            unbound[unbound_count++] = node;
        if (depth < HUB_DEPTH)
            find_below(node, depth + 1);
    }
}

static u32 reset_before(void *node)
{
    u32 i;

    for (i = 0; i < was_reset_count; i++)
        if (was_reset[i] == node)
            return 1;
    return 0;
}

u32 usb_claim_present(void)
{
    u32 c, i, old, reset = 0;

    old = KeRaiseIrqlToDpcLevel();
    seen_count = 0;
    unbound_count = 0;
    for (c = 0; c < 4; c++) {
        void *hub = UsbdGetRootHubDeviceNode(c);
        if (hub)
            find_below(hub, 0);
    }
    KfLowerIrql(old);

    for (i = 0; i < unbound_count; i++) {
        if (reset_before(unbound[i]) || was_reset_count >= UNBOUND_MAX)
            continue;
        was_reset[was_reset_count++] = unbound[i];
        usb_stats.unbound_resets++;
        UsbdResetDevice(unbound[i]);
        reset++;
    }
    return reset;
}
