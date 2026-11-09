/* What runs on a secure link: the profiles (audio/, later hid/) are started from here as
 * they come. Nothing yet: the link is only checked by l2cap.c's echo. */

#include "host.h"

void bt_profiles_ready(volatile BT_CONN *c)
{
    (void)c;
}

void bt_profiles_gone(volatile BT_CONN *c)
{
    (void)c;
}
