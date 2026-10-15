/* The kernel's USB driver interface, for one device.
 *
 *   Match         for every interface of every device that enumerates, ours first
 *                 xerunner: usb-driver-object-f4@1, xerunner: usb-driver-object-f5@1
 *   AddDevice     open the default endpoint, complete synchronously
 *                 xerunner: usb-driver-object-f7@1, xerunner: usb-driver-object-f16@1
 *   RemoveDevice  close every endpoint, last the default one, then complete
 *                 xerunner: usb-driver-object-f18@1, xerunner: usb-transfers-f10@1
 */

#include "usb.h"

USB_DEV usb;
volatile USB_STATS usb_stats;

static USBD_DRIVER_OBJECT driver_object;
static const USB_DRIVER *driver;

static URB close_urb;

/* Requests waiting for the USB processor. */
static KSPIN_LOCK submit_lock;
static URB *submit_head;
static URB *submit_tail;
static KDPC submit_dpc;

#define USB_PROCESSOR 2             /* hardware thread the USB stack runs on */

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

void usb_sleep(u32 ms)
{
    s64 interval = -((s64)ms * 10000);          /* relative, 100 ns units */
    KeDelayExecutionThread(0, 0, &interval);
}

static void submit_now(URB *urb)
{
    if (!usb.device) {
        urb->Busy = 0;
        return;
    }
    mem_sync();                     /* buffer contents visible before the HC looks */
    UsbdQueueAsyncTransfer(usb.device, urb);
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

void usb_prepare(URB *urb, void *endpoint, URB_COMPLETION done, void *buffer,
                 u32 length, const u8 *setup)
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
    urb->Buffer = length ? buffer : 0;
    urb->Length = length;
    if (setup) {
        for (i = 0; i < 8; i++)
            urb->Setup[i] = setup[i];
    }
}

/* --- synchronous control transfers, for the bring-up thread ------------------ */

static URB control_urb;
static volatile u32 control_done;
static volatile u32 control_status;

static void control_complete(URB *urb, u32 status)
{
    control_status = status;
    urb->Busy = 0;
    mem_sync();
    control_done = 1;
}

u32 usb_control(const u8 setup[8], void *buffer, u32 length, u32 *actual,
                u32 timeout_ms)
{
    u32 waited;

    if (!usb.active || !usb.control)
        return STATUS_UNSUCCESSFUL;

    control_done = 0;
    usb_prepare(&control_urb, usb.control, control_complete, buffer, length, setup);
    usb_submit(&control_urb);

    for (waited = 0; !control_done; waited++) {
        if (waited >= timeout_ms)
            return STATUS_UNSUCCESSFUL;
        usb_sleep(1);
    }
    if (actual)
        *actual = control_urb.Actual;
    return control_status;
}

/* --- endpoints ---------------------------------------------------------------- */

void *usb_open(u32 type, u32 address, u32 max_packet, u32 interval)
{
    void *endpoint = 0;

    if (!usb.active || usb.ep_count >= sizeof(usb.ep) / sizeof(usb.ep[0]))
        return 0;
    usb_stats.last_open_status = (u32)UsbdOpenEndpoint(usb.device, type, address,
                                                        max_packet, interval,
                                                        &endpoint);
    if (!endpoint)
        return 0;
    usb.ep[usb.ep_count++] = endpoint;
    return endpoint;
}

/* Each pass closes one endpoint and clears it BEFORE queueing, so the re-entry
 * already sees the next step; a pass that still saw the endpoint it just closed
 * would queue it again forever. */
static void close_complete(URB *urb, u32 status)
{
    void *endpoint;

    (void)status;
    usb_stats.close_passes++;

    while (usb.ep_count > 0) {
        endpoint = usb.ep[usb.ep_count - 1];
        usb.ep[usb.ep_count - 1] = 0;
        usb.ep_count--;
        if (endpoint) {
            usb_prepare(urb, endpoint, close_complete, 0, 0, 0);
            UsbdQueueCloseEndpoint(usb.device, urb);
            return;
        }
    }
    if (usb.control) {
        endpoint = usb.control;
        usb.control = 0;
        usb_prepare(urb, endpoint, close_complete, 0, 0, 0);
        UsbdQueueCloseDefaultEndpoint(usb.device, urb);
        return;
    }

    endpoint = usb.device;
    usb.device = 0;
    UsbdRemoveDeviceComplete(endpoint);
}

/* --- the driver object -------------------------------------------------------- */

static int ours(const u8 *descriptor)
{
    return descriptor && driver && le16(descriptor + 8) == driver->vendor &&
           le16(descriptor + 10) == driver->product;
}

static u32 match(const u8 *interfaceDescriptor, const u8 *deviceDescriptor,
                 USBD_DRIVER_OBJECT *self)
{
    (void)interfaceDescriptor;
    (void)self;
    usb_stats.match_calls++;
    return ours(deviceDescriptor) ? 1u : 0u;
}

static int add_device(void *device)
{
    int status;

    usb_stats.add_calls++;
    if (usb.device || !ours(UsbdGetDeviceDescriptor(device))) {
        UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return (int)STATUS_UNSUCCESSFUL;
    }

    usb.device = device;
    usb.ep_count = 0;
    usb.control = 0;
    status = UsbdOpenDefaultEndpoint(device, &usb.control);
    if (!NT_SUCCESS(status) || !usb.control) {
        usb.control = 0;
        usb.device = 0;
        UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return (int)STATUS_UNSUCCESSFUL;
    }

    usb.generation++;
    mem_sync();
    usb.active = 1;
    UsbdAddDeviceComplete(device, STATUS_SUCCESS);
    if (driver->attached)
        driver->attached();
    return 0;
}

static int remove_device(void *device)
{
    usb_stats.remove_calls++;
    if (device != usb.device) {
        UsbdRemoveDeviceComplete(device);
        return 0;
    }

    usb.active = 0;
    mem_sync();
    if (driver->detached)
        driver->detached();
    close_complete(&close_urb, 0);
    return 0;
}

int usb_register(const USB_DRIVER *drv)
{
    driver = drv;
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
 * PASSIVE_LEVEL, as the debug channel in xdrivers called it. */
u32 usb_claim_present(void)
{
    u32 c, port, old;
    void *found = 0;

    old = KeRaiseIrqlToDpcLevel();
    for (c = 0; c < 4 && !found; c++) {
        void *hub = UsbdGetRootHubDeviceNode(c);
        if (!hub)
            continue;
        for (port = 0; port < 8; port++) {
            void *node = UsbdGetPortDeviceNode(hub, port);
            if (node && ours(UsbdGetDeviceDescriptor(node))) {
                found = node;
                break;
            }
        }
    }
    KfLowerIrql(old);

    if (!found)
        return 0;
    usb_stats.scan_found = (u32)(unsigned long)found;
    usb_stats.reset_status = (u32)UsbdResetDevice(found);
    return 1;
}
