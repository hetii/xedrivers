/* Connections and pairing: the ACL link to one device, Secure Simple Pairing as a device
 * without input or output (Just Works, as a headset pairs), and the link keys it leaves.
 *
 * Either side may call. We call (bt_connect): Create_Connection, then Authentication_
 * Requested, which pairs if the controller has no key from us (Link_Key_Request answered from
 * the table, else negatively), then Set_Connection_Encryption. A device calls us: its
 * Connection_Request is accepted, and the same events follow at its pace.
 *
 * Keys live in bt_keys, read from xedrivers.ini's bt_pair lines (bt_load_keys) and added to
 * when a pairing ends (Link_Key_Notification); `bt_keys_changed` says the ini is behind.
 */

#include "host.h"

volatile BT_CONN bt_conns[BT_CONN_MAX];
volatile BT_KEY bt_keys[BT_KEY_MAX];
volatile u32 bt_keys_changed;

static u32 same(const volatile u8 *a, const volatile u8 *b)
{
    u32 i;

    for (i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void copy6(volatile u8 *to, const u8 *from)
{
    u32 i;

    for (i = 0; i < 6; i++)
        to[i] = from[i];
}

/* Addresses go on the air least significant first; ours are kept as written. */
static void flip(u8 *to, const u8 *from)
{
    u32 i;

    for (i = 0; i < 6; i++)
        to[i] = from[5 - i];
}

volatile BT_CONN *bt_conn_by_address(const u8 *address)
{
    u32 i;

    for (i = 0; i < BT_CONN_MAX; i++)
        if (bt_conns[i].state != BT_CONN_FREE && same(bt_conns[i].address, address))
            return &bt_conns[i];
    return 0;
}

volatile BT_CONN *bt_conn_by_handle(u32 handle)
{
    u32 i;

    for (i = 0; i < BT_CONN_MAX; i++)
        if (bt_conns[i].state >= BT_CONN_UP && bt_conns[i].handle == handle)
            return &bt_conns[i];
    return 0;
}

static volatile BT_CONN *conn_new(const u8 *address)
{
    volatile BT_CONN *c = bt_conn_by_address(address);
    u32 i, k;

    if (c)
        return c;
    for (i = 0; i < BT_CONN_MAX; i++) {
        if (bt_conns[i].state == BT_CONN_FREE) {
            c = &bt_conns[i];
            for (k = 0; k < sizeof(BT_CONN); k++)
                ((volatile u8 *)c)[k] = 0;
            copy6(c->address, address);
            c->state = BT_CONN_PAGING;
            return c;
        }
    }
    return 0;
}

volatile BT_KEY *bt_key_for(const u8 *address)
{
    u32 i;

    for (i = 0; i < BT_KEY_MAX; i++)
        if (bt_keys[i].used && same(bt_keys[i].address, address))
            return &bt_keys[i];
    return 0;
}

void bt_key_store(const u8 *address, const u8 *key, u32 type)
{
    volatile BT_KEY *k = bt_key_for(address);
    u32 i;

    for (i = 0; !k && i < BT_KEY_MAX; i++)
        if (!bt_keys[i].used)
            k = &bt_keys[i];
    if (!k)
        k = &bt_keys[BT_KEY_MAX - 1];   /* full: the last one goes */
    copy6(k->address, address);
    for (i = 0; i < 16; i++)
        k->key[i] = key[i];
    k->type = type;
    k->used = 1;
    bt_keys_changed = 1;
}

/* --- parsing the ini line, pure (tests/bluetooth) ------------------------------------- */

static u32 hex(char c)
{
    if (c >= '0' && c <= '9')
        return (u32)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (u32)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (u32)(c - 'A' + 10);
    return 16;
}

/* "C6:52:BA:8A:3F:4F 0123456789ABCDEF0123456789ABCDEF audio" (the key as BlueZ's info
 * file writes it, most significant byte first, as the controller takes it reversed). */
u32 bt_parse_pair(const char *s, u8 *address, u8 *key, u32 *kind)
{
    u32 i, h, l;

    for (i = 0; i < 6; i++) {
        h = hex(s[0]);
        l = hex(s[1]);
        if (h > 15 || l > 15 || (i < 5 && s[2] != ':'))
            return 0;
        address[i] = (u8)(h << 4 | l);
        s += i < 5 ? 3 : 2;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    for (i = 0; i < 16; i++) {
        h = hex(s[0]);
        l = hex(s[1]);
        if (h > 15 || l > 15)
            return 0;
        key[15 - i] = (u8)(h << 4 | l);
        s += 2;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    *kind = (s[0] == 'h' && s[1] == 'i' && s[2] == 'd') ? BT_KIND_HID : BT_KIND_AUDIO;
    return 1;
}

/* --- what we ask --------------------------------------------------------------------- */

u32 bt_connect(const u8 *address)
{
    volatile BT_CONN *c;
    u8 p[13];

    if (bt_stats.state != BT_STATE_READY)
        return 1;
    c = conn_new(address);
    if (!c)
        return 1;
    if (c->state >= BT_CONN_UP)
        return 0;
    flip(p, address);
    p[6] = 0x18;                        /* packet types: DM1/DH1/DM3/DH3/DM5/DH5 */
    p[7] = 0xCC;
    p[8] = 0x01;                        /* page scan repetition R1 */
    p[9] = 0;
    p[10] = 0;                          /* clock offset unknown */
    p[11] = 0;
    p[12] = 1;                          /* role switch allowed */
    c->we_called = 1;
    c->state = BT_CONN_PAGING;
    if (hci_queue(HCI_CREATE_CONNECTION, p, 13)) {
        c->state = BT_CONN_FREE;
        return 1;
    }
    return 0;
}

u32 bt_disconnect(volatile BT_CONN *c)
{
    u8 p[3];

    if (c->state < BT_CONN_UP)
        return 1;
    p[0] = (u8)c->handle;
    p[1] = (u8)(c->handle >> 8);
    p[2] = 0x13;                        /* remote user terminated */
    return hci_queue(HCI_DISCONNECT, p, 3);
}

static void ask_auth(volatile BT_CONN *c)
{
    u8 p[2];

    p[0] = (u8)c->handle;
    p[1] = (u8)(c->handle >> 8);
    c->state = BT_CONN_AUTH;
    hci_queue(HCI_AUTH_REQUESTED, p, 2);
}

static void ask_encryption(volatile BT_CONN *c)
{
    u8 p[3];

    p[0] = (u8)c->handle;
    p[1] = (u8)(c->handle >> 8);
    p[2] = 1;
    c->state = BT_CONN_ENCRYPTING;
    hci_queue(HCI_SET_ENCRYPTION, p, 3);
}

/* --- what the controller says -------------------------------------------------------- */

void bt_pair_event(const u8 *e, u32 length)
{
    volatile BT_CONN *c;
    volatile BT_KEY *k;
    u8 address[6], p[23];
    u32 i;

    switch (e[0]) {
    case HCI_EV_CONNECTION_REQUEST:             /* address, class, link type */
        if (length < 2 + 10)
            return;
        flip(address, e + 2);
        bt_stats.incoming++;
        if (e[2 + 9] != 1) {                    /* SCO, eSCO: CVSD from linear PCM, as hfp.c */
            static const u8 sync[15] = { 0x40, 0x1F, 0, 0, 0x40, 0x1F, 0, 0,
                                         0xFF, 0xFF, 0x60, 0x00, 0xFF, 0xFF, 0x03 };
            u8 q[21];

            for (i = 0; i < 6; i++)
                q[i] = e[2 + i];
            for (i = 0; i < 15; i++)
                q[6 + i] = sync[i];
            hci_queue(HCI_ACCEPT_SYNC, q, 21);
            return;
        }
        c = conn_new(address);
        for (i = 0; i < 6; i++)
            p[i] = e[2 + i];
        p[6] = 0x01;                            /* stay peripheral: the headset leads */
        if (c)
            hci_queue(HCI_ACCEPT_CONNECTION, p, 7);
        break;
    case HCI_EV_CONNECTION_COMPLETE:            /* status, handle, address, type, enc */
        if (length < 2 + 11)
            return;
        flip(address, e + 5);
        c = bt_conn_by_address(address);
        if (!c)
            return;
        c->last_status = e[2];
        if (e[2]) {
            c->state = BT_CONN_FREE;
            bt_stats.connect_failures++;
            return;
        }
        c->handle = (u32)(e[3] | (e[4] & 0x0F) << 8);
        c->state = BT_CONN_UP;
        c->connects++;
        bt_stats.connections++;
        if (c->we_called)
            ask_auth(c);                        /* pairs, or proves the key we have */
        break;
    case HCI_EV_DISCONNECTION_COMPLETE:         /* status, handle, reason */
        if (length < 2 + 4)
            return;
        c = bt_conn_by_handle((u32)(e[3] | (e[4] & 0x0F) << 8));
        if (c) {
            c->last_status = e[5];
            c->state = BT_CONN_FREE;
            bt_l2cap_gone(c);
        }
        break;
    case HCI_EV_LINK_KEY_REQUEST:               /* address */
        if (length < 2 + 6)
            return;
        flip(address, e + 2);
        k = bt_key_for(address);
        for (i = 0; i < 6; i++)
            p[i] = e[2 + i];
        if (k) {
            for (i = 0; i < 16; i++)
                p[6 + i] = k->key[i];
            hci_queue(HCI_LINK_KEY_REPLY, p, 22);
        } else {
            hci_queue(HCI_LINK_KEY_NEGATIVE, p, 6);
        }
        break;
    case HCI_EV_LINK_KEY_NOTIFICATION:          /* address, key, type */
        if (length < 2 + 23)
            return;
        flip(address, e + 2);
        bt_key_store(address, e + 8, e[24]);
        bt_stats.pairings++;
        break;
    case HCI_EV_IO_CAPABILITY_REQUEST:          /* address */
        if (length < 2 + 6)
            return;
        for (i = 0; i < 6; i++)
            p[i] = e[2 + i];
        p[6] = 0x03;                            /* no input, no output */
        p[7] = 0x00;                            /* no OOB data */
        p[8] = 0x04;                            /* general bonding, no MITM */
        hci_queue(HCI_IO_CAPABILITY_REPLY, p, 9);
        break;
    case HCI_EV_USER_CONFIRMATION_REQUEST:      /* address, value: Just Works, yes */
        if (length < 2 + 6)
            return;
        for (i = 0; i < 6; i++)
            p[i] = e[2 + i];
        hci_queue(HCI_USER_CONFIRMATION_REPLY, p, 6);
        break;
    case HCI_EV_PIN_CODE_REQUEST:               /* a legacy device: "0000", as headsets */
        if (length < 2 + 6)
            return;
        for (i = 0; i < 6; i++)
            p[i] = e[2 + i];
        p[6] = 4;
        for (i = 0; i < 16; i++)
            p[7 + i] = i < 4 ? '0' : 0;
        hci_queue(HCI_PIN_CODE_REPLY, p, 23);
        break;
    case HCI_EV_AUTH_COMPLETE:                  /* status, handle */
        if (length < 2 + 3)
            return;
        c = bt_conn_by_handle((u32)(e[3] | (e[4] & 0x0F) << 8));
        if (!c)
            return;
        c->last_status = e[2];
        if (e[2]) {
            bt_stats.auth_failures++;
            if (e[2] == 0x06)                   /* key missing on its side: pair anew */
                bt_key_forget(c->address);
            bt_disconnect(c);
            return;
        }
        ask_encryption(c);
        break;
    case HCI_EV_ENCRYPTION_CHANGE:              /* status, handle, on */
        if (length < 2 + 4)
            return;
        c = bt_conn_by_handle((u32)(e[3] | (e[4] & 0x0F) << 8));
        if (!c)
            return;
        c->last_status = e[2];
        if (!e[2] && e[5]) {
            c->state = BT_CONN_SECURE;
            c->encrypted = 1;
            bt_l2cap_ready(c);
        }
        break;
    default:
        break;
    }
}

void bt_key_forget(const volatile u8 *address)
{
    u32 i;

    for (i = 0; i < BT_KEY_MAX; i++)
        if (bt_keys[i].used && same(address, bt_keys[i].address))
            bt_keys[i].used = 0;
    bt_keys_changed = 1;
}
