/* Frames of joining and their elements (frames.c), for the SoftMAC station. */

#ifndef XE_WL_FRAMES_H
#define XE_WL_FRAMES_H

#include "softmac.h"

/* What the AP's beacon or probe response says the link may use. */
typedef struct {
    u8  ht;
    u8  vht;
    u8  wmm;
    u8  second_above;           /* HT operation: secondary channel above, below */
    u8  second_below;
    u8  vht_width;              /* VHT operation: 0 20/40, 1 80 */
    u8  vht_center;             /* its center channel */
    u8  ht_sgi20;
    u8  ht_sgi40;
    u8  vht_sgi80;
    u8  ampdu_factor;
    u8  ampdu_density;
    u32 legacy_rates;
    u16 ht_mcs;
    u16 vht_mcs_map;
    u8  rx_ldpc;
    u8  rx_stbc;
} WL_AP_IES;

#define WL_FRAME_MAX 400        /* the longest management frame built here */

u32  wl_frame_probe_req(u8 *f, const u8 *own, u32 seq, u32 channel);
u32  wl_frame_auth(u8 *f, const u8 *own, const u8 *bssid, u32 seq);
u32  wl_frame_deauth(u8 *f, const u8 *own, const u8 *bssid, u32 seq, u32 reason);
u32  wl_frame_assoc_req(u8 *f, const u8 *own, const WL_BSS *bss, const WL_AP_IES *ap,
                        const u8 *rsn, u32 rsn_len, u32 seq);

/* Block ack (ba.c): our request for `tid`, and our answer to the AP's request. */
u32  wl_frame_addba_req(u8 *f, const u8 *own, const u8 *bssid, u32 token, u32 tid,
                        u32 ssn);
u32  wl_frame_addba_resp(u8 *f, const u8 *own, const u8 *bssid, u32 token, u32 status,
                         u32 param, u32 timeout);

/* The elements of a beacon or probe response (after its 24-byte header and 12
 * fixed bytes). */
void wl_parse_ap(const u8 *ies, u32 length, WL_AP_IES *ap);

/* The link the station uses with that AP. */
void wl_link_from_ap(const WL_BSS *bss, const WL_AP_IES *ap, WL_LINK_INFO *link);

/* The link's rate as set up, Mbit/s: what the station reports as its speed. */
u32 wl_link_mbps(const WL_LINK_INFO *link);

#endif /* XE_WL_FRAMES_H */
