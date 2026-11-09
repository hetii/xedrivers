/* The link keys kept in xedrivers.ini, one device a line:
 *
 *     bt_pair = C6:52:BA:8A:3F:4F 0123456789ABCDEF0123456789ABCDEF audio
 *     bt_pair1 = ...                  (and so on to bt_pair7)
 *
 * the key as BlueZ's /var/lib/bluetooth/<adapter>/<device>/info writes it, so a device paired
 * on a PC can be moved over (with the console's controller set to that adapter's address --
 * later). A pairing on the console writes them back (bt_save_keys): every other line of the
 * file stays as it was, in its order. Thread-only.
 */

#include "host.h"
#include "file.h"

#define INI      "xedrivers.ini"
#define INI_MAX  4096u

static const char names[BT_KEY_MAX][9] = {
    "bt_pair", "bt_pair1", "bt_pair2", "bt_pair3",
    "bt_pair4", "bt_pair5", "bt_pair6", "bt_pair7",
};

static char before[INI_MAX], after[INI_MAX + BT_KEY_MAX * 80];

void bt_load_keys(void)
{
    char line[96];
    u8 address[6], key[16];
    u32 i, kind;

    for (i = 0; i < BT_KEY_MAX; i++) {
        if (!file_setting(INI, names[i], line, sizeof(line)))
            continue;
        if (bt_parse_pair(line, address, key, &kind)) {
            bt_key_store(address, key, 4);
            bt_stats.keys_loaded++;
        }
    }
    bt_keys_changed = 0;
}

static u32 put(char *out, u32 at, const char *s)
{
    while (*s && at < sizeof(after) - 1)
        out[at++] = *s++;
    return at;
}

static u32 put_hex(char *out, u32 at, u32 v)
{
    static const char digits[] = "0123456789ABCDEF";

    if (at + 2 < sizeof(after)) {
        out[at++] = digits[(v >> 4) & 15u];
        out[at++] = digits[v & 15u];
    }
    return at;
}

/* Whether the line at `p` sets bt_pair or bt_pairN. */
static u32 is_pair_line(const char *p, u32 n)
{
    static const char key[] = "bt_pair";
    u32 i;

    while (n && (*p == ' ' || *p == '\t')) {
        p++;
        n--;
    }
    for (i = 0; i < 7; i++)
        if (i >= n || p[i] != key[i])
            return 0;
    if (i < n && p[i] >= '1' && p[i] <= '7')
        i++;
    while (i < n && (p[i] == ' ' || p[i] == '\t'))
        i++;
    return i < n && p[i] == '=';
}

u32 bt_save_keys(void)
{
    void *h = file_open(INI);
    u32 length = 0, got, at = 0, out = 0, i, k, slot = 0;

    if (h) {
        do {
            got = file_read(h, before + length, INI_MAX - length, length);
            length += got;
        } while (got && length < INI_MAX);
        file_close(h);
    }
    while (at < length) {
        u32 end = at;

        while (end < length && before[end] != '\n')
            end++;
        if (!is_pair_line(before + at, end - at))
            for (k = at; k <= end && k < length; k++)
                if (out < sizeof(after) - 1)
                    after[out++] = before[k];
        at = end + 1;
    }
    if (out && after[out - 1] != '\n')
        out = put(after, out, "\r\n");
    for (i = 0; i < BT_KEY_MAX; i++) {
        if (!bt_keys[i].used)
            continue;
        out = put(after, out, names[slot++]);
        out = put(after, out, " = ");
        for (k = 0; k < 6; k++) {
            out = put_hex(after, out, bt_keys[i].address[k]);
            if (k < 5)
                out = put(after, out, ":");
        }
        out = put(after, out, " ");
        for (k = 0; k < 16; k++)
            out = put_hex(after, out, bt_keys[i].key[15 - k]);
        out = put(after, out, " audio\r\n");
    }
    bt_keys_changed = 0;
    if (file_replace(INI, after, out)) {
        bt_stats.keys_save_failed++;
        return 1;
    }
    bt_stats.keys_saved++;
    return 0;
}
