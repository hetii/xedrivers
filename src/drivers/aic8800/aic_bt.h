/* The AIC8800D80's Bluetooth half; see aic_bt.c. */

#ifndef XE_AIC_BT_H
#define XE_AIC_BT_H

#include "kernel.h"

struct AIC;

/* Hand the controller to the Bluetooth host. Thread-only; 0, or AIC_STEP_BT_RESET. */
u32 aic_bt_start(struct AIC *a);
void aic_bt_stop(struct AIC *a);

#endif /* XE_AIC_BT_H */
