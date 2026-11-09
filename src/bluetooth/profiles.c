/* What runs on a link: the SDP server and the profiles (audio/ now, hid/ later) are set up
 * once a controller is there, and a link that turns secure starts what its device is for. */

#include "host.h"
#include "a2dp.h"
#include "hfp.h"

void sdp_init(void);

void bt_profiles_init(void)
{
    sdp_init();
    a2dp_init();
    avrcp_init();
    hfp_init();
}

/* A link we made goes on to the audio stream. One the device made is left to it for 3 s (a
 * headset opens AVDTP itself, after asking SDP); then, if it has not, we do: 027570
 * reconnecting by itself opened HFP and AVRCP but not A2DP. */
#define A2DP_WAIT_TICKS 500             /* 10 s: the headset usually leads, given time */

static volatile BT_CONN *waiting;
static u32 waited;

void bt_profiles_ready(volatile BT_CONN *c)
{
    if (c->we_called) {
        a2dp_connect(c);
        return;
    }
    waiting = c;
    waited = 0;
}

void bt_profiles_gone(volatile BT_CONN *c)
{
    if (waiting == c)
        waiting = 0;
    a2dp_gone(c);
    hfp_gone(c);
}

void bt_profiles_tick(void)
{
    if (waiting && ++waited >= A2DP_WAIT_TICKS) {
        if (waiting->state == BT_CONN_SECURE && a2dp_stats.state == A2DP_IDLE)
            a2dp_connect(waiting);
        waiting = 0;
    }
    a2dp_tick();
    hfp_tick();
}

void bt_profiles_event(const u8 *e, u32 length)
{
    hfp_sco_event(e, length);
}

u32 bt_profiles_command(u32 what)
{
    (void)what;
    return hfp_voice();
}
