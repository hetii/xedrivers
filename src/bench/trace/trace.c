/* A recorder whose record outlives the console: the events that change the link
 * (a driver's link report with its caller, an uplink switch, a stalled transmit
 * queue, a station's link down with its caller) and a heartbeat of the counters,
 * written as text to trace.txt beside the plugin -- at once after an event, else
 * every TRACE_EVERY ticks. Made for a hang that left nothing to read over the
 * network: the file is read from the USB medium afterwards (on the bench, the
 * Orange Pi's image). bench=trace.
 */

#include "trace.h"
#include "file.h"
#include "net.h"
#include "xnet.h"

#define TRACE_EVENTS 128
#define TRACE_EVERY  1800               /* ~60 s of worker ticks */

#define GENERIC_WRITE                0x40000000u
#define SYNCHRONIZE                  0x00100000u
#define FILE_OVERWRITE_IF            0x00000005u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define FILE_NON_DIRECTORY_FILE      0x00000040u
#define OBJ_CASE_INSENSITIVE         0x00000040u

/* NtWriteFile (255), kernel.h: eight arguments, all in registers. */

typedef struct {
    u32 tick, code, a, b, c;
} TRACE_EVENT;

static volatile TRACE_EVENT events[TRACE_EVENTS];
static volatile u32 event_count;
static volatile u32 urgent;
static volatile u32 now_tick;
volatile u32 trace_writes, trace_write_status;

void trace_event(u32 code, u32 a, u32 b, u32 c)
{
    u32 i = __sync_fetch_and_add(&event_count, 1) % TRACE_EVENTS;

    events[i].tick = now_tick;
    events[i].code = code;
    events[i].a = a;
    events[i].b = b;
    events[i].c = c;
    mem_sync();
    urgent = 1;
}

static char text[TRACE_EVENTS * 48 + 1024];

static u32 put(u32 at, const char *s)
{
    while (*s && at + 1 < sizeof(text))
        text[at++] = *s++;
    return at;
}

static u32 put_hex(u32 at, u32 v)
{
    static const char digits[] = "0123456789abcdef";
    u32 i;

    for (i = 0; i < 8 && at + 1 < sizeof(text); i++)
        text[at++] = digits[(v >> (28 - 4 * i)) & 15];
    return at;
}

static u32 field(u32 at, const char *name, u32 v)
{
    at = put(at, " ");
    at = put(at, name);
    at = put(at, "=");
    return put_hex(at, v);
}

static void *create(void)
{
    char path[128];
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    void *handle = 0;
    u32 args[10], length = 0, i;
    const volatile char *dir = file_stats.plugin_dir;

    if (!dir[0])
        dir = "\\Device\\Mass0\\plugin\\xedrivers";
    for (i = 0; dir[i] && length + 12 < sizeof(path); i++)
        path[length++] = dir[i];
    for (i = 0; "\\trace.txt"[i]; i++)
        path[length++] = "\\trace.txt"[i];
    path[length] = 0;
    name.Length = (u16)length;
    name.MaximumLength = (u16)(length + 1);
    name.Buffer = path;
    attributes.RootDirectory = 0;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    args[0] = (u32)&handle;
    args[1] = GENERIC_WRITE | SYNCHRONIZE;
    args[2] = (u32)&attributes;
    args[3] = (u32)&io;
    args[4] = 0;
    args[5] = 0;
    args[6] = 0;
    args[7] = FILE_OVERWRITE_IF;
    args[8] = FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE;
    args[9] = (u32)&NtCreateFile;
    trace_write_status = kcall9(args);
    return NT_SUCCESS(trace_write_status) ? handle : 0;
}

static void write_out(u32 tick)
{
    u32 at = 0, i, n = event_count, first = n > TRACE_EVENTS ? n - TRACE_EVENTS : 0;
    u32 rx = 0, tx = 0, inflight = 0;
    IO_STATUS_BLOCK io;
    s64 offset = 0;
    void *h;

    for (i = 0; i < NET_MAX; i++) {
        rx += net_stats[i].rx_frames;
        tx += net_stats[i].tx_frames;
        inflight += net_stats[i].tx_inflight;
    }
    at = put(at, "beat");
    at = field(at, "tick", tick);
    at = field(at, "uplink", net_active);
    at = field(at, "switching", net_switching);
    at = field(at, "rx", rx);
    at = field(at, "tx", tx);
    at = field(at, "inflight", inflight);
    at = field(at, "xmit_usb", xnet_stats.xmit_usb);
    at = field(at, "xmit_fallback", xnet_stats.xmit_fallback);
    at = field(at, "xmit_dropped", xnet_stats.xmit_dropped);
    at = field(at, "xq_stalls", xnet_stats.xq_stalls);
    at = field(at, "link_fallback", xnet_stats.link_fallback);
    at = field(at, "events", n);
    at = put(at, "\r\n");
    for (i = first; i < n; i++) {
        volatile TRACE_EVENT *e = &events[i % TRACE_EVENTS];

        at = put(at, "ev");
        at = field(at, "tick", e->tick);
        at = field(at, "code", e->code);
        at = field(at, "a", e->a);
        at = field(at, "b", e->b);
        at = field(at, "c", e->c);
        at = put(at, "\r\n");
    }
    h = create();
    if (!h)
        return;
    io.Status = 0;
    trace_write_status = NtWriteFile(h, 0, 0, 0, &io, text, at, &offset);
    NtClose(h);
    trace_writes++;
}

void trace_tick(u32 tick)
{
    now_tick = tick;
    if (urgent || tick % TRACE_EVERY == 0) {
        urgent = 0;
        write_out(tick);
    }
}
