/* What runs on a link: the SDP server and the profiles (audio/ now, hid/ later) are set up
 * once a controller is there, and a link that turns secure starts what its device is for. */

#include "host.h"
#include "a2dp.h"

void sdp_init(void);

void bt_profiles_init(void)
{
    sdp_init();
    a2dp_init();
}

/* A link we made goes on to the audio stream; one the device made is left to it (a headset
 * opens AVDTP itself, after asking SDP). */
void bt_profiles_ready(volatile BT_CONN *c)
{
    if (c->we_called)
        a2dp_connect(c);
}

void bt_profiles_gone(volatile BT_CONN *c)
{
    a2dp_gone(c);
}

void bt_profiles_tick(void)
{
    a2dp_tick();
}
