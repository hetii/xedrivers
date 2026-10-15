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

#endif /* XE_CDC_H */
