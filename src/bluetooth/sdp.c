/* SDP, the server side (Core 5.4 Vol 3 Part B): what a device that connects asks before it
 * opens a profile's channel. The console offers an A2DP audio source; headsets look for it
 * before they open AVDTP themselves (BlueZ did, at the first pairing, and closed the link
 * when PSM 1 was refused).
 *
 * Records are kept as attribute lists already in data-element form. All three requests are
 * answered -- service search, attribute, search-and-attribute -- with continuation when the
 * client's byte limit is smaller than the answer.
 */

#include "host.h"

#define PSM_SDP             0x0001u

#define PDU_ERROR           0x01u
#define PDU_SEARCH_REQ      0x02u
#define PDU_SEARCH_RSP      0x03u
#define PDU_ATTR_REQ        0x04u
#define PDU_ATTR_RSP        0x05u
#define PDU_SEARCH_ATTR_REQ 0x06u
#define PDU_SEARCH_ATTR_RSP 0x07u

#define ANSWER_MAX          512u

/* One attribute: its ID and its value, an encoded data element. */
typedef struct {
    u16 id;
    u8  length;
    const u8 *value;
} ATTRIBUTE;

typedef struct {
    u32 handle;
    const u16 *uuids;                   /* every UUID16 the record names, for searches */
    u32 uuid_count;
    const ATTRIBUTE *attributes;        /* in ascending ID order */
    u32 attribute_count;
} RECORD;

/* A2DP Source: AudioSource over L2CAP PSM 0x19 / AVDTP 1.3, profile A2DP 1.3. */
static const u8 a2dp_handle[]   = { 0x0A, 0x00, 0x01, 0x00, 0x01 };         /* u32 0x10001 */
static const u8 a2dp_class[]    = { 0x35, 0x03, 0x19, 0x11, 0x0A };
static const u8 a2dp_protocol[] = { 0x35, 0x10,
                                    0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
                                    0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03 };
static const u8 a2dp_browse[]   = { 0x35, 0x03, 0x19, 0x10, 0x02 };         /* public */
static const u8 a2dp_profile[]  = { 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03 };
static const u8 a2dp_name[]     = { 0x25, 0x08, 'X', 'b', 'o', 'x', ' ', '3', '6', '0' };
static const u8 a2dp_features[] = { 0x09, 0x00, 0x01 };                     /* player */

static const ATTRIBUTE a2dp_attributes[] = {
    { 0x0000, sizeof(a2dp_handle), a2dp_handle },
    { 0x0001, sizeof(a2dp_class), a2dp_class },
    { 0x0004, sizeof(a2dp_protocol), a2dp_protocol },
    { 0x0005, sizeof(a2dp_browse), a2dp_browse },
    { 0x0009, sizeof(a2dp_profile), a2dp_profile },
    { 0x0100, sizeof(a2dp_name), a2dp_name },
    { 0x0311, sizeof(a2dp_features), a2dp_features },
};
static const u16 a2dp_uuids[] = { 0x110A, 0x0100, 0x0019, 0x1002, 0x110D };

/* Hands-free audio gateway: RFCOMM channel 1 (audio/hfp.h), HFP 1.8. */
static const u8 hfp_handle[]   = { 0x0A, 0x00, 0x01, 0x00, 0x02 };
static const u8 hfp_class[]    = { 0x35, 0x06, 0x19, 0x11, 0x1F, 0x19, 0x12, 0x03 };
static const u8 hfp_protocol[] = { 0x35, 0x0C,
                                   0x35, 0x03, 0x19, 0x01, 0x00,
                                   0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x01 };
static const u8 hfp_profile[]  = { 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x1E, 0x09, 0x01, 0x08 };
static const u8 hfp_name[]     = { 0x25, 0x0D, 'V', 'o', 'i', 'c', 'e', ' ', 'G', 'a', 't', 'e',
                                   'w', 'a', 'y' };
static const u8 hfp_network[]  = { 0x08, 0x01 };
static const u8 hfp_features[] = { 0x09, 0x00, 0x00 };

static const ATTRIBUTE hfp_attributes[] = {
    { 0x0000, sizeof(hfp_handle), hfp_handle },
    { 0x0001, sizeof(hfp_class), hfp_class },
    { 0x0004, sizeof(hfp_protocol), hfp_protocol },
    { 0x0005, sizeof(a2dp_browse), a2dp_browse },
    { 0x0009, sizeof(hfp_profile), hfp_profile },
    { 0x0100, sizeof(hfp_name), hfp_name },
    { 0x0301, sizeof(hfp_network), hfp_network },
    { 0x0311, sizeof(hfp_features), hfp_features },
};
static const u16 hfp_uuids[] = { 0x111F, 0x1203, 0x0100, 0x0003, 0x1002, 0x111E };

static const RECORD records[] = {
    { 0x00010001u, a2dp_uuids, 5, a2dp_attributes, 7 },
    { 0x00010002u, hfp_uuids, 6, hfp_attributes, 8 },
};
#define RECORDS (sizeof(records) / sizeof(records[0]))

volatile SDP_STATS sdp_stats;

static u8 answer[ANSWER_MAX];           /* the whole answer, before it is cut to size */
static u8 pdu[ANSWER_MAX + 16];

static u32 be16(const u8 *p) { return (u32)(p[0] << 8 | p[1]); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* A data element's header at `p`: its size into *size, the header's length returned, 0 if
 * it does not fit in `n`. */
static u32 element(const u8 *p, u32 n, u32 *size)
{
    u32 kind, index;

    if (!n)
        return 0;
    kind = p[0] >> 3;
    index = p[0] & 7u;
    if (kind == 0) {                    /* nil */
        *size = 0;
        return 1;
    }
    if (index < 5) {
        *size = 1u << index;
        return 1;
    }
    if (index == 5 && n >= 2) {
        *size = p[1];
        return 2;
    }
    if (index == 6 && n >= 3) {
        *size = be16(p + 1);
        return 3;
    }
    if (index == 7 && n >= 5) {
        *size = be32(p + 1);
        return 5;
    }
    return 0;
}

/* A UUID element as 16 bits: UUID16, UUID32 and UUID128 on the Base UUID all reduce. */
static u32 uuid16(const u8 *p, u32 n, u32 *uuid)
{
    u32 size, h = element(p, n, &size);

    if (!h || (p[0] >> 3) != 3 || h + size > n)
        return 0;
    if (size == 2)
        *uuid = be16(p + h);
    else if (size == 4 || size == 16)
        *uuid = be16(p + h + 2);
    else
        return 0;
    return h + size;
}

/* The UUIDs of a ServiceSearchPattern (a sequence): which records name all of them. */
static u32 matching(const u8 *pattern, u32 n, u32 *list, u32 max, u32 *used)
{
    u32 size, h = element(pattern, n, &size), at, r, count = 0, uuids[12], k = 0;

    if (!h || (pattern[0] >> 3) != 6 || h + size > n)
        return 0;
    for (at = h; at < h + size && k < 12;) {
        u32 len = uuid16(pattern + at, h + size - at, &uuids[k]);

        if (!len)
            return 0;
        at += len;
        k++;
    }
    *used = h + size;
    for (r = 0; r < RECORDS && count < max; r++) {
        u32 all = 1, i, j;

        for (i = 0; i < k && all; i++) {
            u32 found = 0;

            for (j = 0; j < records[r].uuid_count; j++)
                if (records[r].uuids[j] == uuids[i])
                    found = 1;
            all = found;
        }
        if (all && k)
            list[count++] = r;
    }
    return count;
}

/* An AttributeIDList: (low, high) ranges, up to 8. */
static u32 ranges(const u8 *p, u32 n, u32 low[8], u32 high[8], u32 *used)
{
    u32 size, h = element(p, n, &size), at, count = 0;

    if (!h || (p[0] >> 3) != 6 || h + size > n)
        return 0;
    for (at = h; at < h + size && count < 8;) {
        if (p[at] == 0x09 && at + 3 <= h + size) {
            low[count] = high[count] = be16(p + at + 1);
            at += 3;
        } else if (p[at] == 0x0A && at + 5 <= h + size) {
            low[count] = be16(p + at + 1);
            high[count] = be16(p + at + 3);
            at += 5;
        } else {
            return 0;
        }
        count++;
    }
    *used = h + size;
    return count;
}

/* One record's attributes in the ranges, as a sequence, at `out`; its length. */
static u32 put_record(const RECORD *rec, const u32 *low, const u32 *high, u32 nr, u8 *out,
                      u32 room)
{
    u32 at = 3, i, k, j;

    if (room < 3)
        return 0;
    for (i = 0; i < rec->attribute_count; i++) {
        const ATTRIBUTE *a = &rec->attributes[i];
        u32 in = 0;

        for (k = 0; k < nr; k++)
            if (a->id >= low[k] && a->id <= high[k])
                in = 1;
        if (!in)
            continue;
        if (at + 3 + a->length > room)
            return 0;
        out[at++] = 0x09;
        out[at++] = (u8)(a->id >> 8);
        out[at++] = (u8)a->id;
        for (j = 0; j < a->length; j++)
            out[at++] = a->value[j];
    }
    out[0] = 0x36;                      /* sequence, 16-bit size */
    out[1] = (u8)((at - 3) >> 8);
    out[2] = (u8)(at - 3);
    return at;
}

static void send_error(L2CAP_CHANNEL *ch, u32 tid, u32 code)
{
    u8 e[7] = { PDU_ERROR, (u8)(tid >> 8), (u8)tid, 0, 2, (u8)(code >> 8), (u8)code };

    sdp_stats.errors++;
    l2cap_send(ch, e, sizeof(e));
}

/* The part of `answer` (of `total`) from `offset` that fits `limit` and the MTU, with the
 * continuation state after it: a PDU of `kind`. */
static void send_part(L2CAP_CHANNEL *ch, u32 kind, u32 tid, u32 total, u32 offset, u32 limit)
{
    u32 room = ch->remote_mtu > 64 ? ch->remote_mtu - 5 - 2 - 3 : 32, n, at, i;

    if (limit > room)
        limit = room;
    n = total - offset < limit ? total - offset : limit;
    pdu[0] = (u8)kind;
    pdu[1] = (u8)(tid >> 8);
    pdu[2] = (u8)tid;
    pdu[5] = (u8)(n >> 8);
    pdu[6] = (u8)n;
    for (i = 0; i < n; i++)
        pdu[7 + i] = answer[offset + i];
    at = 7 + n;
    if (offset + n < total) {
        pdu[at++] = 2;
        pdu[at++] = (u8)((offset + n) >> 8);
        pdu[at++] = (u8)(offset + n);
        sdp_stats.continued++;
    } else {
        pdu[at++] = 0;
    }
    pdu[3] = (u8)((at - 5) >> 8);
    pdu[4] = (u8)(at - 5);
    l2cap_send(ch, pdu, at);
}

/* The continuation state at `p` (after the request's parameters): the offset it carries. */
static u32 continuation(const u8 *p, u32 n, u32 *offset)
{
    if (n < 1)
        return 0;
    if (p[0] == 0) {
        *offset = 0;
        return 1;
    }
    if (p[0] == 2 && n >= 3) {
        *offset = be16(p + 1);
        return 1;
    }
    return 0;
}

static void request(L2CAP_CHANNEL *ch, const u8 *d, u32 n)
{
    u32 kind, tid, used = 0, used2 = 0, list[RECORDS], count, low[8], high[8], nr, limit,
        total = 0, offset, i;
    const u8 *p;

    if (n < 5)
        return;
    kind = d[0];
    tid = be16(d + 1);
    p = d + 5;
    n -= 5;
    sdp_stats.requests++;
    switch (kind) {
    case PDU_SEARCH_REQ:
        count = matching(p, n, list, RECORDS, &used);
        if (n < used + 2 + 1) {
            send_error(ch, tid, 0x0003);
            return;
        }
        limit = be16(p + used);
        if (count > limit)
            count = limit;
        pdu[0] = PDU_SEARCH_RSP;
        pdu[1] = (u8)(tid >> 8);
        pdu[2] = (u8)tid;
        pdu[5] = 0;
        pdu[6] = (u8)count;
        pdu[7] = 0;
        pdu[8] = (u8)count;
        for (i = 0; i < count; i++) {
            u32 h = records[list[i]].handle;

            pdu[9 + 4 * i] = (u8)(h >> 24);
            pdu[10 + 4 * i] = (u8)(h >> 16);
            pdu[11 + 4 * i] = (u8)(h >> 8);
            pdu[12 + 4 * i] = (u8)h;
        }
        pdu[9 + 4 * count] = 0;
        pdu[3] = 0;
        pdu[4] = (u8)(4 + 4 * count + 1);
        l2cap_send(ch, pdu, 10 + 4 * count);
        return;
    case PDU_ATTR_REQ:
        if (n < 4 + 2) {
            send_error(ch, tid, 0x0003);
            return;
        }
        for (i = 0; i < RECORDS && records[i].handle != be32(p); i++)
            ;
        if (i == RECORDS) {
            send_error(ch, tid, 0x0002);            /* no such record */
            return;
        }
        limit = be16(p + 4);
        nr = ranges(p + 6, n - 6, low, high, &used);
        if (!nr || !continuation(p + 6 + used, n - 6 - used, &offset)) {
            send_error(ch, tid, 0x0003);
            return;
        }
        total = put_record(&records[i], low, high, nr, answer, ANSWER_MAX);
        if (offset > total) {
            send_error(ch, tid, 0x0005);            /* bad continuation */
            return;
        }
        send_part(ch, PDU_ATTR_RSP, tid, total, offset, limit);
        return;
    case PDU_SEARCH_ATTR_REQ:
        count = matching(p, n, list, RECORDS, &used);
        if (!used || n < used + 2) {
            send_error(ch, tid, 0x0003);
            return;
        }
        limit = be16(p + used);
        nr = ranges(p + used + 2, n - used - 2, low, high, &used2);
        if (!nr || !continuation(p + used + 2 + used2, n - used - 2 - used2, &offset)) {
            send_error(ch, tid, 0x0003);
            return;
        }
        total = 3;
        for (i = 0; i < count; i++)
            total += put_record(&records[list[i]], low, high, nr, answer + total,
                                ANSWER_MAX - total);
        answer[0] = 0x36;                           /* a sequence of the records' lists */
        answer[1] = (u8)((total - 3) >> 8);
        answer[2] = (u8)(total - 3);
        if (offset > total) {
            send_error(ch, tid, 0x0005);
            return;
        }
        send_part(ch, PDU_SEARCH_ATTR_RSP, tid, total, offset, limit);
        return;
    default:
        send_error(ch, tid, 0x0003);
        return;
    }
}

static void sdp_data(L2CAP_CHANNEL *ch, const u8 *d, u32 n)
{
    request(ch, d, n);
}

static const L2CAP_USER sdp_user = { 0, sdp_data, 0 };

void sdp_init(void)
{
    l2cap_listen(PSM_SDP, &sdp_user, 0);
}
