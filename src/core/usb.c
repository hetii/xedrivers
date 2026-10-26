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


static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

void usb_sleep(u32 ms)
{
    s64 interval = -((s64)ms * 10000);          /* relative, 100 ns units */
    KeDelayExecutionThread(0, 0, &interval);
}

/* The worker's alarm clock. A timed wait alone is not enough: once the dashboard
 * dims the screen after ten idle minutes, a thread waiting only on a timeout here
 * is not woken again until somebody touches a pad (measured: the worker stopped at
 * the same tick every time, and went on as soon as the pad moved). So the worker
 * and the control transfers it waits for also wait on this event, which whatever
 * they wait for sets -- a device arriving or leaving, a control transfer done. */
static KEVENT wake_event;
static volatile u32 wake_ready;

void usb_wake(void)
{
    if (wake_ready)
        KeSetEvent(&wake_event, 0, 0);
}

void usb_wait(u32 ms)
{
    s64 timeout = -((s64)ms * 10000);

    if (!wake_ready) {
        usb_sleep(ms);
        return;
    }
    KeWaitForSingleObject(&wake_event, 0, 0, 0, &timeout);
}

static void submit_now(URB *urb)
{
    if (!urb->Dev || !urb->Dev->device) {
        urb->Busy = 0;
        return;
    }
    mem_sync();                     /* buffer contents visible before the HC looks */
    {
        PROF_START(t);
        UsbdQueueAsyncTransfer(urb->Dev->device, urb);
        PROF_END(submit, t, urb->Length);
    }
}

static void submit_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    URB *list;

    (void)dpc;
    (void)context;
    (void)a1;
    (void)a2;

    usb_stats.dpc_runs++;
    KeAcquireSpinLockAtRaisedIrql(&submit_lock);
    list = submit_head;
    submit_head = 0;
    submit_tail = 0;
    KeReleaseSpinLockFromRaisedIrql(&submit_lock);

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
    urb->Routine = done;
    urb->Endpoint = endpoint;
    urb->Dev = dev;
    urb->Buffer = length ? buffer : 0;
    urb->Length = length;
    if (setup) {
        for (i = 0; i < 8; i++)
            urb->Setup[i] = setup[i];
    }
}

/* --- synchronous control transfers, for the bring-up thread ------------------ */

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

    dev->control_done = 0;
    usb_prepare(&dev->control_urb, dev, dev->control, control_complete, buffer, length,
                setup);
    usb_submit(&dev->control_urb);

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

static u32 match(const u8 *interfaceDescriptor, const u8 *deviceDescriptor,
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

static int add_device(void *device)
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

static int remove_device(void *device)
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

int usb_register(const USB_CLIENT *who)
{
    u32 i;

    for (i = 0; i < USB_MAX_DEVICES; i++)
        usb_devs[i].index = i;
    client = who;
    KeInitializeEvent(&wake_event, 1, 0);
    mem_sync();
    wake_ready = 1;
    KeInitializeDpc(&submit_dpc, submit_dpc_routine, 0);
    submit_dpc.TargetNumber = USB_PROCESSOR + 1;

    /* xerunner: usb-driver-object-f2@1 -- both list words zero or a bug check. */
    driver_object.ListEntry.Flink = 0;
    driver_object.ListEntry.Blink = 0;
    driver_object.Private = 0;
    driver_object.AddDevice = add_device;
    driver_object.RemoveDevice = remove_device;
    driver_object.Match = match;
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
