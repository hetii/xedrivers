/* HCI: the controller brought up, commands sent as it has room for them, its events taken in
 * order, and a scan for devices. See bt.h for who calls what from where.
 *
 * Bring-up, one command after another, each sent when the last one's answer came:
 *   Reset, Read_BD_ADDR, Read_Buffer_Size, Set_Event_Mask, Write_Class_Of_Device,
 *   Write_Local_Name, Write_Inquiry_Mode (2: results with RSSI or extended), Write_Simple_
 *   Pairing_Mode (on), Write_Scan_Enable (page scan: a paired device can call us).
 * A command that fails is counted and the next one goes all the same: none of them is one the
 * host cannot do without but Reset, and that one's failure shows in failed_opcode.
 */

#include "host.h"

#define QUEUE           16u             /* events, a power of two */
#define EVENT_MAX       260u
#define CLASS_OF_DEVICE 0x000804u       /* Toy: game console */
#define INQUIRY_LENGTH  8u              /* x 1.28 s */

volatile BT_STATS bt_stats;
volatile BT_FOUND bt_found[BT_FOUND_MAX];

static const BT_TRANSPORT *transport;
static u8 queue[QUEUE][EVENT_MAX];
static u16 queue_length[QUEUE];
static volatile u32 queue_in, queue_out;
static u32 credits;                     /* commands the controller can take now */
static u32 step;                        /* in the bring-up */
static u32 waiting;                     /* the opcode we wait for, 0 none */
static u8 out[EVENT_MAX];

/* Commands that wait for a credit: whatever the events ask in answer (pair.c) and what
 * xectl asks, in order. */
#define WAITING_MAX 8u
#define WAITING_SIZE 32u                /* the longest such command's parameters */
static u8 waiting_params[WAITING_MAX][WAITING_SIZE];
static u16 waiting_opcode[WAITING_MAX];
static u8 waiting_length[WAITING_MAX];
static u32 waiting_in, waiting_out;
static u32 keys_loaded;

/* --- for the driver ------------------------------------------------------------------- */

void bt_event(const u8 *event, u32 length)
{
    u32 in = queue_in, i;

    if (!transport || length < 2 || length > EVENT_MAX)
        return;
    if (in - queue_out >= QUEUE) {
        bt_stats.events_dropped++;
        return;
    }
    for (i = 0; i < length; i++)
        queue[in & (QUEUE - 1)][i] = event[i];
    queue_length[in & (QUEUE - 1)] = (u16)length;
    mem_sync();
    queue_in = in + 1;
}

/* ACL packets in, queued as the events are (any IRQL in, the worker out). */
#define ACL_QUEUE 8u
#define ACL_MAX   1028u
static u8 acl_queue[ACL_QUEUE][ACL_MAX];
static u16 acl_length[ACL_QUEUE];
static volatile u32 acl_in, acl_out;
static u32 acl_credits;                 /* packets the controller can take now */
static u8 acl_buf[ACL_MAX];

void bt_acl_in(const u8 *packet, u32 length)
{
    u32 in = acl_in, i;

    if (!transport || length < 4 || length > ACL_MAX)
        return;
    if (in - acl_out >= ACL_QUEUE) {
        bt_stats.acl_dropped++;
        return;
    }
    for (i = 0; i < length; i++)
        acl_queue[in & (ACL_QUEUE - 1)][i] = packet[i];
    acl_length[in & (ACL_QUEUE - 1)] = (u16)length;
    mem_sync();
    acl_in = in + 1;
}

u32 hci_acl_send(u32 handle, const u8 *data, u32 length)
{
    u32 i;

    if (!transport || !acl_credits || length + 4 > ACL_MAX || length > bt_stats.acl_mtu)
        return 1;
    acl_buf[0] = (u8)handle;
    acl_buf[1] = (u8)((handle >> 8) & 0x0F) | 0x20;    /* PB 10: a start, flushable */
    acl_buf[2] = (u8)length;
    acl_buf[3] = (u8)(length >> 8);
    for (i = 0; i < length; i++)
        acl_buf[4 + i] = data[i];
    if (transport->acl(transport->ctx, acl_buf, length + 4))
        return 1;
    acl_credits--;
    bt_stats.acl_sent++;
    return 0;
}

void bt_attach(const BT_TRANSPORT *t)
{
    u32 i, k;

    for (i = 0; i < BT_FOUND_MAX; i++)
        for (k = 0; k < sizeof(BT_FOUND); k++)
            ((volatile u8 *)&bt_found[i])[k] = 0;
    queue_out = queue_in;
    acl_out = acl_in;
    acl_credits = 0;
    waiting_in = waiting_out = 0;
    keys_loaded = 0;
    for (i = 0; i < BT_CONN_MAX; i++)
        bt_conns[i].state = BT_CONN_FREE;
    credits = 1;
    step = 0;
    waiting = 0;
    bt_stats.found = 0;
    mem_sync();
    transport = t;
    bt_profiles_init();
    bt_stats.attached = 1;
    bt_stats.state = BT_STATE_INIT;
}

void bt_detach(void)
{
    transport = 0;
    bt_stats.attached = 0;
    bt_stats.state = BT_STATE_DETACHED;
}

/* --- commands ------------------------------------------------------------------------- */

u32 hci_send(u32 opcode, const u8 *params, u32 length)
{
    u32 i;

    if (!transport || !credits || length > 255)
        return 1;
    out[0] = (u8)opcode;
    out[1] = (u8)(opcode >> 8);
    out[2] = (u8)length;
    for (i = 0; i < length; i++)
        out[3 + i] = params[i];
    if (transport->command(transport->ctx, out, length + 3))
        return 1;
    credits--;
    waiting = opcode;
    bt_stats.commands++;
    bt_stats.last_opcode = opcode;
    return 0;
}

u32 hci_queue(u32 opcode, const u8 *params, u32 length)
{
    u32 at, i;

    if (!transport || length > WAITING_SIZE)
        return 1;
    if (waiting_in - waiting_out >= WAITING_MAX) {
        bt_stats.queue_full++;
        return 1;
    }
    at = waiting_in & (WAITING_MAX - 1);
    for (i = 0; i < length; i++)
        waiting_params[at][i] = params[i];
    waiting_opcode[at] = (u16)opcode;
    waiting_length[at] = (u8)length;
    waiting_in++;
    bt_stats.queued = waiting_in - waiting_out;
    return 0;
}

/* As many waiting commands as the controller has credits for, once the bring-up is over. */
static void send_waiting(void)
{
    while (credits && waiting_out != waiting_in && bt_stats.state != BT_STATE_INIT) {
        u32 at = waiting_out & (WAITING_MAX - 1);

        if (hci_send(waiting_opcode[at], waiting_params[at], waiting_length[at]))
            break;
        waiting_out++;
    }
    bt_stats.queued = waiting_in - waiting_out;
}

/* The bring-up's next command; 1 when there is none left. */
static u32 bring_up(u32 at)
{
    static const u8 mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };
    static const char name[] = "Xbox 360";
    u8 p[248];
    u32 i;

    switch (at) {
    case 0: return hci_send(HCI_RESET, 0, 0), 0;
    case 1: return hci_send(HCI_READ_BD_ADDR, 0, 0), 0;
    case 2: return hci_send(HCI_READ_BUFFER_SIZE, 0, 0), 0;
    case 3: return hci_send(HCI_SET_EVENT_MASK, mask, 8), 0;
    case 4:
        p[0] = (u8)CLASS_OF_DEVICE;
        p[1] = (u8)(CLASS_OF_DEVICE >> 8);
        p[2] = (u8)(CLASS_OF_DEVICE >> 16);
        return hci_send(HCI_WRITE_CLASS, p, 3), 0;
    case 5:
        for (i = 0; i < sizeof(p); i++)
            p[i] = i < sizeof(name) - 1 ? (u8)name[i] : 0;
        return hci_send(HCI_WRITE_LOCAL_NAME, p, 248), 0;
    case 6: p[0] = 2; return hci_send(HCI_WRITE_INQUIRY_MODE, p, 1), 0;
    case 7: p[0] = 1; return hci_send(HCI_WRITE_SSP_MODE, p, 1), 0;
    case 8: p[0] = 2; return hci_send(HCI_WRITE_SCAN_ENABLE, p, 1), 0;
    default: return 1;
    }
}

static void start_inquiry(void)
{
    static const u8 p[5] = { 0x33, 0x8B, 0x9E, INQUIRY_LENGTH, 0 };     /* GIAC, no limit */
    u32 i, k;

    for (i = 0; i < BT_FOUND_MAX; i++)
        for (k = 0; k < sizeof(BT_FOUND); k++)
            ((volatile u8 *)&bt_found[i])[k] = 0;
    bt_stats.found = 0;
    if (!hci_send(HCI_INQUIRY, p, 5)) {
        bt_stats.state = BT_STATE_INQUIRY;
        bt_stats.inquiries++;
    }
}

u32 bt_command(u32 what, volatile u32 *args)
{
    u8 address[6];
    u32 i;

    switch (what) {
    case BT_CMD_SCAN:
        args[5] = bt_stats.state;
        if (bt_stats.state == BT_STATE_READY && credits)
            start_inquiry();
        return 1;
    case BT_CMD_CONNECT:
        /* [1] < 16: that one of the scan's; else [1] [2] the address, 4 + 2 bytes */
        if (args[1] < BT_FOUND_MAX) {
            if (args[1] >= bt_stats.found) {
                args[5] = 2;
                return 1;
            }
            for (i = 0; i < 6; i++)
                address[i] = bt_found[args[1]].address[i];
        } else {
            for (i = 0; i < 4; i++)
                address[i] = (u8)(args[1] >> (24 - 8 * i));
            address[4] = (u8)(args[2] >> 8);
            address[5] = (u8)args[2];
        }
        args[5] = bt_connect(address);
        return 1;
    case BT_CMD_DISCONNECT:
        args[5] = args[1] < BT_CONN_MAX ? bt_disconnect(&bt_conns[args[1]]) : 2;
        return 1;
    default:
        return 0;
    }
}

/* --- events --------------------------------------------------------------------------- */

static volatile BT_FOUND *slot_for(const u8 *address_le)
{
    u32 i, k;

    for (i = 0; i < bt_stats.found; i++) {
        for (k = 0; k < 6 && bt_found[i].address[k] == address_le[5 - k]; k++)
            ;
        if (k == 6)
            return &bt_found[i];
    }
    if (bt_stats.found >= BT_FOUND_MAX)
        return 0;
    for (k = 0; k < 6; k++)
        bt_found[bt_stats.found].address[k] = address_le[5 - k];
    return &bt_found[bt_stats.found++];
}

u32 hci_eir_name(const u8 *eir, u32 length, char *name, u32 size)
{
    u32 at = 0, n, i;

    while (at + 1 < length && eir[at]) {
        n = eir[at];
        if (at + 1 + n > length)
            break;
        if ((eir[at + 1] == 0x09 || eir[at + 1] == 0x08) && size) {     /* complete, short */
            for (i = 0; i + 1 < n && i + 1 < size; i++)
                name[i] = (char)eir[at + 2 + i];
            name[i] = 0;
            return i;
        }
        at += 1 + n;
    }
    return 0;
}

void hci_inquiry_result(const u8 *e, u32 length)
{
    u32 count, i, code = e[0];
    const u8 *p = e + 3;

    if (length < 3)
        return;
    /* With more than one response the parameters come as arrays, field by field; controllers
     * send one a time, and only the first is read. */
    count = e[2] ? 1 : 0;
    for (i = 0; i < count; i++) {
        volatile BT_FOUND *f;
        u32 each = 14;                  /* 0x02: ..., class at 9; 0x22 and 0x2F: class at 8, RSSI 13 */

        if (p + each > e + length)
            return;
        f = slot_for(p);
        if (!f)
            return;
        f->seen++;
        if (code == HCI_EV_INQUIRY_RESULT) {
            f->device_class = p[9] | (u32)p[10] << 8 | (u32)p[11] << 16;
        } else {
            f->device_class = p[8] | (u32)p[9] << 8 | (u32)p[10] << 16;
            f->rssi = (s8)p[13];
        }
        if (code == HCI_EV_EXTENDED_INQUIRY && length >= 3 + 14 + 1) {
            char name[32];

            if (hci_eir_name(p + 14, length - 3 - 14, name, sizeof(name))) {
                u32 k;

                for (k = 0; k < sizeof(name); k++)
                    f->name[k] = name[k];
            }
        }
        p += each;
    }
}

/* The answer to a command: its opcode, its status, what it returned. */
static void complete(u32 opcode, u32 status, const u8 *r, u32 n)
{
    u32 i;

    bt_stats.last_status = status;
    if (status)
        bt_stats.failed_opcode = opcode;
    if (opcode == HCI_READ_BD_ADDR && !status && n >= 6) {
        for (i = 0; i < 6; i++)
            ((volatile u8 *)bt_stats.address)[i] = r[5 - i];
    } else if (opcode == HCI_READ_BUFFER_SIZE && !status && n >= 7) {
        bt_stats.acl_mtu = r[0] | (u32)r[1] << 8;
        bt_stats.acl_slots = r[3] | (u32)r[4] << 8;
        acl_credits = bt_stats.acl_slots;
    }
    if (opcode == waiting)
        waiting = 0;
    if (bt_stats.state == BT_STATE_INIT) {
        step++;
        if (bring_up(step) && step > 8)
            bt_stats.state = BT_STATE_READY;
    }
}

static void take(const u8 *e, u32 length)
{
    bt_stats.events++;
    switch (e[0]) {
    case HCI_EV_COMMAND_COMPLETE:
        if (length >= 6) {
            credits = e[2];
            complete((u32)(e[3] | e[4] << 8), e[5], e + 6, length - 6);
        }
        break;
    case HCI_EV_COMMAND_STATUS:
        if (length >= 6) {
            credits = e[3];
            if (e[2]) {                 /* refused at once */
                u32 opcode = (u32)(e[4] | e[5] << 8), i;

                bt_stats.failed_opcode = opcode;
                bt_stats.last_status = e[2];
                if (opcode == HCI_INQUIRY && bt_stats.state == BT_STATE_INQUIRY)
                    bt_stats.state = BT_STATE_READY;
                if (opcode == HCI_CREATE_CONNECTION) {
                    bt_stats.connect_failures++;
                    for (i = 0; i < BT_CONN_MAX; i++)
                        if (bt_conns[i].state == BT_CONN_PAGING)
                            bt_conns[i].state = BT_CONN_FREE;
                }
            }
        }
        break;
    case HCI_EV_INQUIRY_RESULT:
    case HCI_EV_INQUIRY_RESULT_RSSI:
    case HCI_EV_EXTENDED_INQUIRY:
        hci_inquiry_result(e, length);
        break;
    case HCI_EV_NUMBER_COMPLETED:       /* count, then handle and packets for each */
        if (length >= 3) {
            u32 n = e[2], i;

            for (i = 0; i < n && 3 + 4 * i + 4 <= length; i++)
                acl_credits += (u32)(e[3 + 4 * i + 2] | e[3 + 4 * i + 3] << 8);
            if (acl_credits > bt_stats.acl_slots)
                acl_credits = bt_stats.acl_slots;
        }
        break;
    case HCI_EV_INQUIRY_COMPLETE:
        if (bt_stats.state == BT_STATE_INQUIRY)
            bt_stats.state = BT_STATE_READY;
        break;
    default:
        bt_pair_event(e, length);
        break;
    }
}

void bt_tick(void)
{
    if (!transport)
        return;
    if (bt_stats.state == BT_STATE_INIT && step == 0 && !waiting && credits)
        bring_up(0);
    if (bt_stats.state == BT_STATE_READY && !keys_loaded) {
        keys_loaded = 1;
        bt_load_keys();
    }
    while (queue_out != queue_in) {
        u32 at = queue_out & (QUEUE - 1);

        take(queue[at], queue_length[at]);
        mem_sync();
        queue_out++;
    }
    while (acl_out != acl_in) {
        u32 at = acl_out & (ACL_QUEUE - 1);

        bt_stats.acl_received++;
        l2cap_acl(acl_queue[at], acl_length[at]);
        mem_sync();
        acl_out++;
    }
    send_waiting();
    if (bt_keys_changed && keys_loaded)
        bt_save_keys();
    bt_profiles_tick();
}
