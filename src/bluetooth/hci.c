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

void bt_acl_in(const u8 *packet, u32 length)
{
    (void)packet;
    (void)length;                       /* nothing connects yet: l2cap.c */
}

void bt_attach(const BT_TRANSPORT *t)
{
    u32 i, k;

    for (i = 0; i < BT_FOUND_MAX; i++)
        for (k = 0; k < sizeof(BT_FOUND); k++)
            ((volatile u8 *)&bt_found[i])[k] = 0;
    queue_out = queue_in;
    credits = 1;
    step = 0;
    waiting = 0;
    bt_stats.found = 0;
    mem_sync();
    transport = t;
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
    if (what != BT_CMD_SCAN)
        return 0;
    args[5] = bt_stats.state;
    if (bt_stats.state == BT_STATE_READY && credits)
        start_inquiry();
    return 1;
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
                bt_stats.failed_opcode = (u32)(e[4] | e[5] << 8);
                bt_stats.last_status = e[2];
                if (bt_stats.state == BT_STATE_INQUIRY)
                    bt_stats.state = BT_STATE_READY;
            }
        }
        break;
    case HCI_EV_INQUIRY_RESULT:
    case HCI_EV_INQUIRY_RESULT_RSSI:
    case HCI_EV_EXTENDED_INQUIRY:
        hci_inquiry_result(e, length);
        break;
    case HCI_EV_INQUIRY_COMPLETE:
        if (bt_stats.state == BT_STATE_INQUIRY)
            bt_stats.state = BT_STATE_READY;
        break;
    default:
        break;
    }
}

void bt_tick(void)
{
    if (!transport)
        return;
    if (bt_stats.state == BT_STATE_INIT && step == 0 && !waiting && credits)
        bring_up(0);
    while (queue_out != queue_in) {
        u32 at = queue_out & (QUEUE - 1);

        take(queue[at], queue_length[at]);
        mem_sync();
        queue_out++;
    }
}
