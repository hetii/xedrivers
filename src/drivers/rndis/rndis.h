/* RNDIS messages and the descriptors of an RNDIS function. Pure functions over
 * bytes, no kernel calls, so tests/ compiles this file natively and checks it
 * against what a real phone sent.
 *
 * Remote NDIS is Microsoft's "Ethernet over USB": control messages go through the
 * default endpoint as CDC SEND_ENCAPSULATED_COMMAND / GET_ENCAPSULATED_RESPONSE on
 * the communication interface, every frame on the bulk pair is wrapped in a 44-byte
 * REMOTE_NDIS_PACKET_MSG header. All fields are little-endian. Shapes from
 * [MS-RNDIS] and Linux rndis_host.c (GPL-2.0), which is what Android phones are
 * tested against.
 */

#ifndef XE_RNDIS_H
#define XE_RNDIS_H

#include "kernel.h"

#define RNDIS_MSG_PACKET      0x00000001u
#define RNDIS_MSG_INIT        0x00000002u
#define RNDIS_MSG_HALT        0x00000003u
#define RNDIS_MSG_QUERY       0x00000004u
#define RNDIS_MSG_SET         0x00000005u
#define RNDIS_MSG_RESET       0x00000006u
#define RNDIS_MSG_INDICATE    0x00000007u
#define RNDIS_MSG_KEEPALIVE   0x00000008u
#define RNDIS_MSG_COMPLETION  0x80000000u   /* or-ed into the reply's type */

#define RNDIS_STATUS_SUCCESS          0x00000000u
#define RNDIS_STATUS_MEDIA_CONNECT    0x4001000Bu
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000Cu

#define OID_GEN_LINK_SPEED             0x00010107u   /* units of 100 bit/s */
#define OID_GEN_MEDIA_CONNECT_STATUS   0x00010114u   /* 0 connected, 1 not */
#define OID_GEN_CURRENT_PACKET_FILTER  0x0001010Eu
#define OID_802_3_PERMANENT_ADDRESS    0x01010101u

/* DIRECTED | ALL_MULTICAST | BROADCAST | PROMISCUOUS, as Linux rndis_host: the
 * consumer above filters by destination itself. */
#define RNDIS_FILTER_DEFAULT 0x0000002Du

#define RNDIS_PACKET_HEADER  44u
#define RNDIS_CONTROL_MAX    1025u      /* what Linux reads a response into */

/* Where the RNDIS function sits in a configuration. */
typedef struct {
    u8  config_value;
    u8  comm_interface;     /* the one control messages are addressed to */
    u8  data_interface;
    u8  data_alt;
    u8  ep_in;
    u8  ep_out;
    u8  ep_intr;            /* RESPONSE_AVAILABLE notifications; may be 0 */
    u16 in_max;
    u16 out_max;
    u16 intr_max;
    u8  intr_interval;
} RNDIS_LAYOUT;

/* Whether an interface descriptor's class triple is an RNDIS control interface:
 * E0/01/03 (Android and most phones), 02/02/FF (Windows' own and Linux gadgets of
 * old), EF/04/01 (the miscellaneous-class form). */
int rndis_is_control(u32 class_code, u32 subclass, u32 protocol);

/* Fill `out` from one whole configuration descriptor. Returns 1 when it holds an
 * RNDIS control interface and a data interface with a bulk pair. */
int rndis_parse_config(const u8 *config, u32 length, RNDIS_LAYOUT *out);

/* Control messages into `buf`; each returns the length to send. */
u32 rndis_build_init(u8 *buf, u32 request, u32 max_transfer);
u32 rndis_build_query(u8 *buf, u32 request, u32 oid);
u32 rndis_build_set(u8 *buf, u32 request, u32 oid, const u8 *value, u32 length);
u32 rndis_build_keepalive_reply(u8 *buf, u32 request);

/* A reply: its type, request id and status, and for a query where the answer is.
 * Returns 1 if `buf` holds a well-formed message of at least the header. */
typedef struct {
    u32 type;
    u32 request;
    u32 status;
    const u8 *info;         /* query completion: the information buffer */
    u32 info_length;
    /* init completion */
    u32 max_packets;        /* MaxPacketsPerTransfer */
    u32 max_transfer;       /* MaxTransferSize: the largest the device accepts */
    u32 alignment;          /* PacketAlignmentFactor, as a power of two */
} RNDIS_REPLY;

int rndis_parse_reply(const u8 *buf, u32 length, RNDIS_REPLY *out);

/* Wrap one Ethernet frame for the bulk OUT endpoint: header then frame, into
 * `buf`. Returns the transfer length, 0 if it does not fit. */
u32 rndis_wrap(u8 *buf, u32 buf_size, const u8 *frame, u32 length);

/* The same, for packing several into one transfer: the message is padded with
 * zeros to a multiple of `align` (a power of two, from the device's
 * PacketAlignmentFactor) and its MessageLength says so, so the next one starts
 * aligned. Returns the padded length, 0 if it does not fit. */
u32 rndis_wrap_aligned(u8 *buf, u32 buf_size, const u8 *frame, u32 length,
                       u32 align);

/* Every frame in one bulk IN transfer; a device may pack several PACKET_MSGs into
 * one. Returns the number of frames, or -1 at the first malformed message (the
 * frames before it are delivered). */
typedef void (*RNDIS_FRAME)(const u8 *frame, u32 length, void *context);
int rndis_unwrap(const u8 *buf, u32 length, RNDIS_FRAME each, void *context);

#endif /* XE_RNDIS_H */
