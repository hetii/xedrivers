/* How the driver runs, in one place.
 *
 * Chosen at build time (`make MODE=xnet ADVERTISE=2500 IP=10.36.0.2`, see the
 * Makefile) and changeable while it runs through the debug channel
 * (tests/xectl.py mode|advertise ...). `xe_config` lives at a fixed address in the
 * image, so it can also simply be read back to see what a running plugin is doing.
 */

#ifndef XE_CONFIG_H
#define XE_CONFIG_H

#include "kernel.h"

/* Who gets the frames off the card. */
#define XE_MODE_DROP       0u
#define XE_MODE_STANDALONE 1u   /* the built-in responder on xe_config.ip */
#define XE_MODE_XNET       2u   /* the console's own network stack */

#ifndef XE_MODE
#define XE_MODE XE_MODE_XNET
#endif

/* What the PHY advertises, as RTL_ADV_* bits (rtl.h); 0 leaves the card's own
 * firmware default, which in CDC-ECM is 10/100 only. */
#ifndef XE_ADVERTISE
#define XE_ADVERTISE 0x6Fu      /* 10/100/1000/2500 */
#endif

/* How frames cross USB: CDC ECM (one frame per transfer) or CDC NCM (many frames
 * per transfer block). Changing it needs a new bring-up: xectl.py transport ncm
 * writes it and resets the card. */
#define XE_TRANSPORT_ECM 0u
#define XE_TRANSPORT_NCM 1u

#ifndef XE_TRANSPORT
#define XE_TRANSPORT XE_TRANSPORT_ECM
#endif

#ifndef XE_IP
#define XE_IP 0x0A240002u       /* 10.36.0.2 */
#endif

/* The RAM-to-RAM TCP server (src/bench/tcp/), built only with `make bench=tcp`: it is what
 * makes the plugin import from xam, and the driver needs none of it. */
#ifndef XE_TCP
#define XE_TCP 0
#endif

typedef struct {
    u32 magic;                  /* 'XCFG' */
    u32 mode;                   /* XE_MODE_* the worker applies */
    u32 advertise;              /* RTL_ADV_* applied at every bring-up */
    u32 ip;                     /* standalone address, host order */
    u32 transport;              /* XE_TRANSPORT_* used at the next bring-up */
} XE_CONFIG;

extern volatile XE_CONFIG xe_config;

#endif /* XE_CONFIG_H */
