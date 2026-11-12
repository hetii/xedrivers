/* Loading the AIC8800D80's firmware into a chip in boot ROM: aic_load_fw's
 * system_config_8800d80 and aicfw_download_fw_8800d80, normal mode, in order.
 *
 *   1. the chip id from 0x40500000; an MCU part also gets its cache enabled
 *   2. fw_patch_table: where the patches go (its first, "PINF", section), then
 *      the ADID and patch images and any extra patch, then every section's
 *      address/value pairs written one by one, the BT mode section with the
 *      driver's settings put in
 *   3. the Wi-Fi firmware (fmacfw) at 0x120000, its patch configuration, and
 *      DBG_START_APP: the chip leaves the bus and comes back running it.
 *
 * The files are those Linux loads from /lib/firmware/aic8800D80, looked for in
 * firmware\\aic8800 beside the plugin (file.h). Revision U01 chips, which need
 * other files, are only recognised.
 */

#include "aic.h"
#include "file.h"

#define DIR                 FIRMWARE_DIR "aic8800\\"
#define PATCH_TABLE         DIR "fw_patch_table_8800d80_u02.bin"
#define ADID                DIR "fw_adid_8800d80_u02.bin"
#define PATCH               DIR "fw_patch_8800d80_u02.bin"
#define EXT_PATCH           DIR "fw_patch_8800d80_u02_ext"     /* + id + ".bin" */
#define FMAC                DIR "fmacfw_8800d80_u02.bin"
#define FMAC_H              DIR "fmacfw_8800d80_h_u02.bin"

#define CHIP_WORD           0x40500000u
#define CACHE_WORD          0x40100020u
#define CHIP_REV_U01        0x01u
#define CHIP_ID_H_MASK      0xC0u
#define FMAC_ADDR_U02       0x120000u
#define PATCH_START_DEFAULT 0x001D7000u

/* Patch table sections (AICBT_PT_*). */
#define PT_INF              0u
#define PT_BTMODE           3u
#define PT_PWRON            4u
#define PT_VER              6u

#define TABLE_MAX           4096u
#define EXT_MAX             4u

static u8 table[TABLE_MAX];         /* one table at a time: a chip loads once */

/* What the PINF section says. */
typedef struct {
    u32 addr_adid;
    u32 addr_patch;
    u32 ext_count;
    u32 ext_id[EXT_MAX];
    u32 ext_addr[EXT_MAX];
} PATCH_INFO;

static u32 fail(AIC *a, u32 step, u32 status)
{
    a->st->step = step;
    a->st->step_status = status;
    return step;
}

/* Read a whole small file into `table`. Returns its size, 0 if missing or too big. */
static u32 read_small(AIC *a, const char *name)
{
    void *h = file_open(name);
    u32 size = 0, got;

    if (!h) {
        a->st->file_status = file_stats.last_status;
        return 0;
    }
    do {
        got = file_read(h, table + size, TABLE_MAX - size, size);
        size += got;
    } while (got && size < TABLE_MAX);
    file_close(h);
    return size < TABLE_MAX ? size : 0;
}

/* rwnx_plat_bin_fw_upload_android: the file in 1 KB blocks from `address` on. */
static u32 upload(AIC *a, u32 address, const char *name)
{
    void *h = file_open(name);
    u32 at = 0, got, status = STATUS_SUCCESS;

    if (!h) {
        a->st->file_status = file_stats.last_status;
        return STATUS_UNSUCCESSFUL;
    }
    while ((got = file_read(h, a->file_buf, AIC_BLOCK, at)) != 0) {
        status = aic_block_write(a, address + at, a->file_buf, got);
        if (!NT_SUCCESS(status))
            break;
        at += got;
        a->st->bytes_loaded += got;
    }
    file_close(h);
    if (NT_SUCCESS(status) && !at)
        status = STATUS_UNSUCCESSFUL;               /* an empty file */
    if (NT_SUCCESS(status))
        a->st->files_loaded++;
    return status;
}

/* A section: 16 bytes of name, type, count, then count address/value pairs (none
 * for a type of 1000 or more). Calls `each` per section; 0 if the table is bad. */
typedef u32 (*SECTION_FN)(AIC *a, u32 type, u8 *pairs, u32 count, void *context);

static u32 sections(AIC *a, u32 size, SECTION_FN each, void *context)
{
    static const char tag[] = "AICBT_PT_TAG";
    u32 at = 16, i, type, count;

    for (i = 0; tag[i]; i++)
        if (table[i] != (u8)tag[i])
            return 0;
    while (at + 24 <= size) {
        type = aic_le32(table + at + 16);
        count = aic_le32(table + at + 20);
        at += 24;
        if (type >= 1000)
            count = 0;
        if (at + count * 8 > size)
            return 0;
        if (!each(a, type, table + at, count, context))
            return 0;
        at += count * 8;
    }
    return 1;
}

/* aicbt_patch_info_unpack, of the PINF section, which comes first: four pairs
 * (ADID and patch addresses, a reset, a flag), then the extra patch count, then an
 * id and an address per extra patch. */
static u32 unpack_info(AIC *a, u32 type, u8 *pairs, u32 count, void *context)
{
    PATCH_INFO *info = (PATCH_INFO *)context;
    u32 i;

    (void)a;
    if (type != PT_INF || info->addr_adid)
        return 1;
    if (count < 2)
        return 0;
    info->addr_adid = aic_le32(pairs + 4);
    info->addr_patch = aic_le32(pairs + 12);
    if (count > 4) {
        info->ext_count = aic_le32(pairs + 4 * 8 + 4);
        if (info->ext_count > EXT_MAX || 5 + info->ext_count > count)
            return 0;
        for (i = 0; i < info->ext_count; i++) {
            info->ext_id[i] = aic_le32(pairs + (5 + i) * 8);
            info->ext_addr[i] = aic_le32(pairs + (5 + i) * 8 + 4);
        }
    }
    return 1;
}

static u32 bt_txpwr(u32 linux_value)
{
    char v[16];
    u32 n = file_setting("xedrivers.ini", "bt_txpwr", v, sizeof(v)), i, x = 0;

    if (n < 3 || v[0] != '0' || (v[1] | 0x20) != 'x')
        return linux_value;
    for (i = 2; i < n; i++) {
        char c = v[i];

        x = x << 4 | (u32)(c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return x;
}

/* aicbt_patch_table_load: every pair written; the PINF section only its four base
 * pairs (unpack trims its length); the BT mode section with the driver's values in
 * the odd words; the version text skipped; 100 ms after the power-on section. */
static u32 load_section(AIC *a, u32 type, u8 *pairs, u32 count, void *context)
{
    u32 i, status;

    (void)context;
    if (type == PT_VER)
        return 1;
    if (type == PT_INF && count > 4)
        count = 4;
    if (type == PT_BTMODE && count >= 9) {
        aic_put32(pairs + 1 * 4, 1);            /* hwinfo < 0: none given */
        aic_put32(pairs + 3 * 4, 0xFFFFFFFFu);  /* hwinfo -1 */
        aic_put32(pairs + 5 * 4, 0);            /* cpmode: work */
        aic_put32(pairs + 7 * 4, 5);            /* btmode: BT only, no external switch */
        aic_put32(pairs + 9 * 4, 1);            /* btport: MB */
        aic_put32(pairs + 11 * 4, 1500000);     /* uart baud */
        aic_put32(pairs + 13 * 4, 1);           /* uart flow control */
        aic_put32(pairs + 15 * 4, 0);           /* low power mode off */
        /* Bluetooth's transmit power (0x201FE0): 0x6F2F as Linux writes it (usbmon,
         * load-8d80.txt), over the table's 0x5F2F7F2F; bt_txpwr = 0x... in the ini to try
         * others. */
        aic_put32(pairs + 17 * 4, bt_txpwr(0x6F2F));
    }
    for (i = 0; i < count; i++) {
        status = aic_mem_write(a, aic_le32(pairs + i * 8), aic_le32(pairs + i * 8 + 4));
        if (!NT_SUCCESS(status)) {
            a->st->step_status = status;
            return 0;
        }
    }
    if (type == PT_PWRON)
        usb_sleep(100);
    return 1;
}

/* aicwf_patch_config_8800d80: the firmware's patch block, read off its image, gets
 * the three settings Linux's default build makes (2.4 GHz band flags, receive
 * aggregation counter 10, power offsets over calibration) and no patch blocks. */
static const u32 config_pairs[][2] = {
    { 0x00B4, 0xF3010000u },
    { 0x0170, 0x0001000Au },        /* receive aggregation; PLATFORM_HI's 0x00010001
                                     * was tried and changed nothing measurable */
    { 0x0188, 0x00000001u },
};

static u32 patch_config(AIC *a)
{
    u32 base_addr = FMAC_ADDR_U02 + 0x198, config_base, patch_base, version, start;
    u32 i, status, count = sizeof(config_pairs) / sizeof(config_pairs[0]);

    status = aic_mem_read(a, base_addr, &config_base);
    if (NT_SUCCESS(status))
        status = aic_mem_read(a, base_addr + 8, &patch_base);
    if (NT_SUCCESS(status))
        status = aic_mem_read(a, FMAC_ADDR_U02 + 0x1C, &version);
    if (!NT_SUCCESS(status))
        return status;
    a->st->fw_version = version;
    start = PATCH_START_DEFAULT;
    if (version > 0x06090100u) {
        status = aic_mem_read(a, base_addr + 12, &start);
        if (!NT_SUCCESS(status))
            return status;
    }
    /* aic_patch_t: magic, pair_start, magic_2, pair_count, block_dst[4],
     * block_src[4], block_size[4]. */
    status = aic_mem_write(a, patch_base + 0, 0x48435450u);         /* "PTCH" */
    if (NT_SUCCESS(status))
        status = aic_mem_write(a, patch_base + 8, 0x50544348u);     /* "HCTP" */
    if (NT_SUCCESS(status))
        status = aic_mem_write(a, patch_base + 4, start);
    if (NT_SUCCESS(status))
        status = aic_mem_write(a, patch_base + 12, count);
    for (i = 0; NT_SUCCESS(status) && i < count; i++) {
        status = aic_mem_write(a, start + 8 * i, config_pairs[i][0] + config_base);
        if (NT_SUCCESS(status))
            status = aic_mem_write(a, start + 8 * i + 4, config_pairs[i][1]);
    }
    for (i = 0; NT_SUCCESS(status) && i < 4; i++)
        status = aic_mem_write(a, patch_base + 48 + 4 * i, 0);
    return status;
}

static void name_ext(char *to, u32 id)
{
    const char *p = EXT_PATCH;
    u32 i = 0;

    while (*p)
        to[i++] = *p++;
    if (id >= 10)
        to[i++] = (char)('0' + id / 10 % 10);
    to[i++] = (char)('0' + id % 10);
    to[i++] = '.';
    to[i++] = 'b';
    to[i++] = 'i';
    to[i++] = 'n';
    to[i] = 0;
}

u32 aic_load_firmware(AIC *a)
{
    PATCH_INFO info;
    char ext[64];
    u32 word = 0, status, size, i;

    status = aic_mem_read(a, CHIP_WORD, &word);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_CHIP, status);
    a->st->chip_word = word;
    a->st->chip_id = (word >> 16) & 0xFF;
    a->st->mcu_id = ((word >> 25) & 1) == 0;
    if (a->st->mcu_id) {
        status = aic_mem_read(a, CACHE_WORD, &word);
        if (NT_SUCCESS(status))
            status = aic_mem_write(a, CACHE_WORD, word | 1);
        if (!NT_SUCCESS(status))
            return fail(a, AIC_STEP_CHIP, status);
    }
    if (a->st->chip_id == CHIP_REV_U01)
        return fail(a, AIC_STEP_CHIP, a->st->chip_id);

    for (i = 0; i < sizeof(info) / 4; i++)
        ((u32 *)&info)[i] = 0;
    size = read_small(a, PATCH_TABLE);
    if (!size || !sections(a, size, unpack_info, &info) || !info.addr_adid)
        return fail(a, AIC_STEP_PATCH_TABLE, size);

    status = upload(a, info.addr_adid, ADID);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_ADID, status);
    status = upload(a, info.addr_patch, PATCH);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_PATCH, status);
    for (i = 0; i < info.ext_count; i++) {
        name_ext(ext, info.ext_id[i]);
        status = upload(a, info.ext_addr[i], ext);
        if (!NT_SUCCESS(status))
            return fail(a, AIC_STEP_EXT_PATCH, status);
    }
    /* The table again, to load: its PINF and BT mode sections were read above
     * unchanged, so the same bytes serve. */
    if (!sections(a, size, load_section, 0))
        return fail(a, AIC_STEP_TABLE_LOAD, a->st->step_status);

    status = upload(a, FMAC_ADDR_U02,
                    (a->st->chip_id & CHIP_ID_H_MASK) == CHIP_ID_H_MASK ? FMAC_H : FMAC);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_FMAC, status);
    status = patch_config(a);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_PATCH_CONFIG, status);
    status = aic_start_app(a, FMAC_ADDR_U02, AIC_START_APP_AUTO);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_START_APP, status);
    a->st->step = AIC_STEP_FIRMWARE_STARTED;
    return 0;
}
