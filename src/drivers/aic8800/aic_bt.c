/* The AIC8800D80's Bluetooth controller, reached through the same device node as its Wi-Fi:
 * the composite device's first interface is standard HCI over USB (class e0/01/01), which
 * needs no vendor step once aic_load_fw has put the firmware in. So all of it is the common
 * transport's (drivers/common/btusb) and the host's (src/bluetooth); this only hands the node
 * over, saying which endpoints the Wi-Fi side has.
 */

#include "aic.h"
#include "btusb.h"

static BTUSB bt;

u32 aic_bt_start(AIC *a)
{
    /* the Wi-Fi's: commands 0x02/0x82, data 0x01/0x81 (aic_cmd.c, aic_data.c) */
    static const u8 wifi[] = { 0x02, 0x82, 0x01, 0x81, 0 };

    return btusb_start(&bt, a->dev, wifi) ? AIC_STEP_BT_RESET : 0;
}

void aic_bt_stop(AIC *a)
{
    (void)a;
    btusb_stop(&bt);
}
