/* AVRCP over AVCTP (PSM 0x17), the console as the target a headset's buttons talk to: enough
 * that a headset which opens it is answered (027570 opened it before A2DP, and a refusal is
 * not what headsets expect). UNIT and SUBUNIT INFO as a panel; PASS THROUGH (play, pause,
 * next...) accepted and kept in the stats for later use; everything else NOT IMPLEMENTED, as
 * the AV/C rules allow. The browsing channel (0x1B) is opened and left silent.
 */

#include "host.h"

#define PSM_AVCTP          0x0017u
#define PSM_AVCTP_BROWSING 0x001Bu

#define NOT_IMPLEMENTED 0x08u
#define ACCEPTED        0x09u
#define STABLE          0x0Cu

volatile AVRCP_STATS avrcp_stats;

static void data(L2CAP_CHANNEL *ch, const u8 *d, u32 n)
{
    u8 r[64];
    u32 i, len;

    if (n < 3 || (d[0] & 0x02))             /* commands only */
        return;
    avrcp_stats.commands++;
    len = n < sizeof(r) ? n : sizeof(r);
    for (i = 0; i < len; i++)
        r[i] = d[i];
    r[0] = (u8)((d[0] & 0xF0u) | 0x02u);    /* the same label, a response */
    if (d[1] != 0x11 || d[2] != 0x0E) {     /* not AV remote control: invalid profile */
        r[0] |= 0x01;
        l2cap_send(ch, r, 3);
        return;
    }
    if (n < 6) {
        return;
    }
    switch (d[5]) {
    case 0x30:                              /* UNIT INFO */
    case 0x31:                              /* SUBUNIT INFO */
        r[3] = STABLE;
        r[6] = 0x07;
        r[7] = 0x48;                        /* panel, 0 */
        r[8] = 0xFF; r[9] = 0xFF; r[10] = 0xFF;
        len = 11;
        break;
    case 0x7C:                              /* PASS THROUGH: a button */
        r[3] = ACCEPTED;
        if (n >= 7) {
            avrcp_stats.last_button = d[6];
            if (!(d[6] & 0x80))
                avrcp_stats.presses++;
        }
        break;
    default:
        r[3] = NOT_IMPLEMENTED;
        avrcp_stats.not_implemented++;
        break;
    }
    l2cap_send(ch, r, len);
}

static const L2CAP_USER control_user = { 0, data, 0 };
static const L2CAP_USER browsing_user = { 0, 0, 0 };

void avrcp_init(void)
{
    l2cap_listen(PSM_AVCTP, &control_user, 0);
    l2cap_listen(PSM_AVCTP_BROWSING, &browsing_user, 0);
}
