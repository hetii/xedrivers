/* Inside the SoftMAC station: what its files share (softmac.h is what it offers). */

#ifndef XE_WL_STATION_H
#define XE_WL_STATION_H

#include "softmac.h"

/* data.c: a data frame from the AP, and wpa.c's host's send. */
void wl_sta_rx_data(WL_STA *s, u8 *frame, u32 length, u32 decrypted);
u32  wl_sta_send_eapol(void *context, const u8 *body, u32 length);

/* ba.c: block-ack sessions. */
void wl_sta_action(WL_STA *s, const u8 *frame, u32 length);
void wl_sta_ba_init(WL_STA *s);
void wl_sta_ba_reset(WL_STA *s);
void wl_sta_ba_tick(WL_STA *s);

#endif /* XE_WL_STATION_H */
