/* CDC descriptor parsing and notification decoding. Pure functions over bytes, no
 * kernel calls, so tests/ compiles this file natively and checks it against the
 * descriptors read off the real card. */

#ifndef XE_CDC_H
#define XE_CDC_H

#include "kernel.h"

#define CDC_SUBCLASS_ECM 0x06
#define CDC_SUBCLASS_NCM 0x0D

typedef struct {
    u8  config_value;       /* bConfigurationValue to select */
    u8  subclass;           /* CDC_SUBCLASS_ECM or _NCM */
    u8  comm_interface;
    u8  data_interface;
    u8  data_alt;           /* the alternate setting that has the bulk pair */
    u8  ep_in;
    u8  ep_out;
    u8  ep_intr;
    u16 in_max;
    u16 out_max;
    u16 intr_max;
    u8  intr_interval;
    u8  mac_string;         /* iMACAddress */
    u16 max_segment;        /* wMaxSegmentSize */
} CDC_LAYOUT;

/* Fill `out` from one whole configuration descriptor (wTotalLength bytes). Returns
 * 1 when it describes a CDC ECM or NCM function with a usable data interface. */
int cdc_parse_config(const u8 *config, u32 length, CDC_LAYOUT *out);

/* A USB string descriptor holding twelve hex digits, UTF-16LE, to six bytes. */
int cdc_parse_mac(const u8 *string, u32 length, u8 mac[6]);

/* What an interrupt-endpoint notification says. */
enum { CDC_NOTE_NONE, CDC_NOTE_LINK, CDC_NOTE_SPEED };
typedef struct {
    u32 kind;
    u32 link_up;
    u32 down_bps;
    u32 up_bps;
} CDC_NOTE;

/* Decode one notification; returns the number of bytes it used (0 if too short),
 * since a device may put two back to back in one transfer. */
u32 cdc_parse_notification(const u8 *data, u32 length, CDC_NOTE *note);

/* --- NCM (CDC NCM 1.0, NTB16 only) ------------------------------------------- */

/* GET_NTB_PARAMETERS, 28 bytes. */
typedef struct {
    u16 formats;            /* bmNtbFormatsSupported: bit 0 NTB16, bit 1 NTB32 */
    u32 in_max;             /* dwNtbInMaxSize: the largest NTB the device sends */
    u16 in_divisor;
    u16 in_remainder;
    u16 in_alignment;
    u32 out_max;            /* dwNtbOutMaxSize: the largest it accepts */
    u16 out_divisor;        /* datagram offset % divisor == remainder */
    u16 out_remainder;
    u16 out_alignment;      /* NDP offset alignment */
    u16 out_max_datagrams;  /* 0 = no limit */
} NCM_PARAMS;

int cdc_ncm_parse_params(const u8 *data, u32 length, NCM_PARAMS *out);

/* Walk one received NTB16 and call `each` for every datagram in it, in order.
 * Returns the number of datagrams, or -1 when the block is malformed (whatever was
 * delivered before the fault stays delivered). */
typedef void (*NCM_DATAGRAM)(const u8 *frame, u32 length, void *context);
int cdc_ncm_parse_ntb(const u8 *ntb, u32 length, NCM_DATAGRAM each, void *context);

/* Building one NTB16 to send. The NDP goes either right after the 12-byte header
 * with room for `NCM_TX_MAX_DATAGRAMS` entries and the datagrams after it, or --
 * `ndp_end` -- after the last datagram. Each datagram is placed at an offset the
 * device's divisor/remainder allows. */
#ifndef NCM_TX_MAX_DATAGRAMS
#define NCM_TX_MAX_DATAGRAMS 32
#endif

typedef struct {
    u8  *buf;
    u32  capacity;          /* min(buffer, out_max) */
    u32  ndp_end;           /* NDP after the datagrams (Linux's NDP_TO_END) */
    u16  alignment;         /* wNdpOutAlignment */
    u16  entry[NCM_TX_MAX_DATAGRAMS][2];
    u32  used;              /* end of the last datagram */
    u32  count;
    u16  divisor;
    u16  remainder;
    u16  max_datagrams;
    u16  sequence;
} NCM_TX;

void cdc_ncm_tx_begin(NCM_TX *tx, u8 *buf, u32 buf_size, const NCM_PARAMS *p,
                      u16 sequence, u32 ndp_end);
/* Append one frame; 0 if it does not fit (the NTB is then left as it was). */
int  cdc_ncm_tx_add(NCM_TX *tx, const u8 *frame, u32 length);
/* Write header and NDP; returns the bytes to send. A block that is a multiple of
 * `max_packet` would need a zero-length packet to end it, so it grows by one
 * unused byte instead (the header's wBlockLength includes it). */
u32  cdc_ncm_tx_finish(NCM_TX *tx, u32 max_packet);

#endif /* XE_CDC_H */
