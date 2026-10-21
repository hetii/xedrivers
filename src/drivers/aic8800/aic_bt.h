/* The AIC8800D80's Bluetooth half; see aic_bt.c. */

#ifndef XE_AIC_BT_H
#define XE_AIC_BT_H

#include "kernel.h"

struct AIC;

#define AIC_BT_COMMAND_MAX  260     /* three header bytes and up to 255 parameters */

/* What the controller said about itself; read by tests/xectl.py. */
typedef struct {
    u32 up;
    u32 commands;
    u32 command_errors;
    u32 timeouts;
    u32 events;
    u32 event_errors;
    u32 last_event;
    u32 last_event_error;
    u32 last_opcode;
    u32 reset_status;
    u32 version_status;
    u32 address_status;
    u32 hci_version;
    u32 hci_revision;
    u32 lmp_version;
    u32 manufacturer;
    u32 address[2];             /* bytes in order, as a Bluetooth address reads */
} AIC_BT_STATS;

extern volatile AIC_BT_STATS aic_bt_stats;

/* Bring the controller up and read its identity. Thread-only; 0, or the step that
 * failed. */
u32 aic_bt_start(struct AIC *a);
void aic_bt_stop(struct AIC *a);

#endif /* XE_AIC_BT_H */
