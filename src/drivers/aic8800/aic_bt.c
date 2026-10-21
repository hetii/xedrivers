/* The AIC8800D80's Bluetooth controller, reached through the same device node as
 * its Wi-Fi: the composite device's first interface is standard HCI over USB
 * (class e0/01/01), which needs no vendor step once aic_load_fw has put the
 * firmware in. Commands are class control requests to the device; the controller's
 * answers arrive on interrupt endpoint 0x83, 64 bytes at a time.
 *
 * All this does is bring the controller up and ask who it is: a reset, its version
 * and its address. That is enough to say whether the console can drive this half
 * of the card at all, which is what was not known. Nothing is connected or paired.
 */

#include "aic.h"
#include "aic_bt.h"

#define EVENT_MAX       64u         /* the interrupt endpoint's packet */

/* An HCI command: three bytes of header, then its parameters. The opcode is the
 * group and the command within it. */
#define OP(group, command)  ((group) << 10 | (command))
#define HCI_RESET           OP(0x03, 0x0003)
#define HCI_READ_VERSION    OP(0x04, 0x0001)
#define HCI_READ_ADDRESS    OP(0x04, 0x0009)

#define EV_COMPLETE         0x0Eu   /* a command finished, with its answer */

volatile AIC_BT_STATS aic_bt_stats;

static URB event_urb;
static u8 *event_buf;
static u8 *command_buf;
static AIC *owner;
static volatile u32 waiting_for;    /* the opcode whose answer we want */
static volatile u32 answered;
static u8 answer[32];
static u32 answer_len;

static void event_queue(void);

/* An event from the controller. The first byte is its code, the second how much
 * follows; a command-complete event then carries the slots the controller has
 * free, the opcode it finished, and that command's own answer. */
static void event_complete(URB *urb, u32 status)
{
    const u8 *e = event_buf;
    u32 opcode, i;

    if (!owner || !owner->bt_running || !owner->dev->active)
        return;
    if (!NT_SUCCESS(status)) {
        aic_bt_stats.event_errors++;
        aic_bt_stats.last_event_error = status;
        if (++owner->bt_errors < AIC_RX_ERROR_LIMIT)
            event_queue();
        return;
    }
    owner->bt_errors = 0;
    if (urb->Actual >= 2 && (u32)e[1] + 2 <= urb->Actual) {
        aic_bt_stats.events++;
        aic_bt_stats.last_event = e[0];
        if (e[0] == EV_COMPLETE && e[1] >= 3) {
            opcode = (u32)(e[3] | e[4] << 8);
            aic_bt_stats.last_opcode = opcode;
            if (opcode == waiting_for && !answered) {
                answer_len = e[1] - 3 < sizeof(answer) ? e[1] - 3 : sizeof(answer);
                for (i = 0; i < answer_len; i++)
                    answer[i] = e[5 + i];
                mem_sync();
                answered = 1;
                usb_wake();
            }
        }
    }
    event_queue();
}

static void event_queue(void)
{
    usb_prepare(&event_urb, owner->dev, owner->ep_bt_event, event_complete,
                event_buf, EVENT_MAX, 0);
    usb_submit(&event_urb);
}

/* One command, and the answer the controller sends back for it. Thread-only: it
 * waits. Returns its first byte, which every command uses as its status, or
 * 0xFF when nothing came. */
static u32 command(u32 opcode, const u8 *params, u32 length)
{
    static const u8 setup_template[8] = { 0x20, 0x00, 0, 0, 0, 0, 0, 0 };
    u8 setup[8];
    u32 waited, i;

    if (length + 3 > AIC_BT_COMMAND_MAX)
        return 0xFF;
    command_buf[0] = (u8)opcode;
    command_buf[1] = (u8)(opcode >> 8);
    command_buf[2] = (u8)length;
    for (i = 0; i < length; i++)
        command_buf[3 + i] = params[i];
    for (i = 0; i < 8; i++)
        setup[i] = setup_template[i];
    setup[6] = (u8)(length + 3);
    setup[7] = 0;

    answered = 0;
    waiting_for = opcode;
    mem_sync();
    aic_bt_stats.commands++;
    if (!NT_SUCCESS(usb_control(owner->dev, setup, command_buf, length + 3, 0,
                                1000))) {
        aic_bt_stats.command_errors++;
        return 0xFF;
    }
    for (waited = 0; !answered; waited++) {
        if (waited >= 1000 || !owner->dev->active) {
            aic_bt_stats.timeouts++;
            waiting_for = 0;
            return 0xFF;
        }
        usb_wait(1);
    }
    waiting_for = 0;
    return answer_len ? answer[0] : 0;
}

u32 aic_bt_start(AIC *a)
{
    u32 status, i;

    if (!a->ep_bt_event) {
        if (!command_buf) {
            command_buf = (u8 *)net_dma_scratch(AIC_BT_COMMAND_MAX);
            event_buf = (u8 *)net_dma_scratch(EVENT_MAX);
        }
        if (!command_buf || !event_buf)
            return AIC_STEP_DMA;
        /* The event endpoint's interval is 1 ms at high speed; the kernel polls
         * no faster than it can, which is plenty for commands. */
        a->ep_bt_event = usb_open(a->dev, USB_TYPE_INTERRUPT, 0x83, EVENT_MAX, 1);
        if (!a->ep_bt_event)
            return AIC_STEP_ENDPOINTS;
    }
    owner = a;
    a->bt_errors = 0;
    a->bt_running = 1;
    mem_sync();
    event_queue();

    status = command(HCI_RESET, 0, 0);
    aic_bt_stats.reset_status = status;
    if (status)
        return AIC_STEP_BT_RESET;

    status = command(HCI_READ_VERSION, 0, 0);
    aic_bt_stats.version_status = status;
    if (!status && answer_len >= 9) {
        aic_bt_stats.hci_version = answer[1];
        aic_bt_stats.hci_revision = (u32)(answer[2] | answer[3] << 8);
        aic_bt_stats.lmp_version = answer[4];
        aic_bt_stats.manufacturer = (u32)(answer[5] | answer[6] << 8);
    }

    status = command(HCI_READ_ADDRESS, 0, 0);
    aic_bt_stats.address_status = status;
    if (!status && answer_len >= 7)
        for (i = 0; i < 6; i++)             /* the controller gives it least first */
            ((volatile u8 *)aic_bt_stats.address)[i] = answer[6 - i];

    aic_bt_stats.up = 1;
    return 0;
}

void aic_bt_stop(AIC *a)
{
    a->bt_running = 0;
    aic_bt_stats.up = 0;
    mem_sync();
}
