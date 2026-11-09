/* The link keys kept in xedrivers.ini, one device a line:
 *
 *     bt_pair = C6:52:BA:8A:3F:4F 0123456789ABCDEF0123456789ABCDEF audio
 *     bt_pair1 = ...                  (and so on to bt_pair7)
 *
 * the key as BlueZ's /var/lib/bluetooth/<adapter>/<device>/info writes it, so a device paired
 * on a PC can be moved over (with the console's controller set to that adapter's address --
 * later). Thread-only.
 */

#include "host.h"
#include "file.h"

void bt_load_keys(void)
{
    static const char names[BT_KEY_MAX][9] = {
        "bt_pair", "bt_pair1", "bt_pair2", "bt_pair3",
        "bt_pair4", "bt_pair5", "bt_pair6", "bt_pair7",
    };
    char line[96];
    u8 address[6], key[16];
    u32 i, kind;

    for (i = 0; i < BT_KEY_MAX; i++) {
        if (!file_setting("xedrivers.ini", names[i], line, sizeof(line)))
            continue;
        if (bt_parse_pair(line, address, key, &kind)) {
            bt_key_store(address, key, 4);
            bt_stats.keys_loaded++;
        }
    }
    bt_keys_changed = 0;
}
