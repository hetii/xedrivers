/* Send NTBs built by src/cdc.c through the host's RTL8156 in its NCM configuration,
 * over usbfs, so the transmit format can be tried against the real card firmware
 * without rebooting the console. A box on the far side of the cable (the Orange Pi)
 * watches for the test frames:
 *
 *     tests/usbcfg.py 2                       card on NCM, under cdc_ncm
 *     build/ncm_inject <ndp_end> <len> [count] [skip_setup]
 *     ssh opi tcpdump -ni end0 ether proto 0x88b5
 *     tests/usbcfg.py reset                   back to r8152
 *
 * It detaches cdc_ncm, claims both interfaces, and repeats the plugin's bring-up:
 * data interface alt 0, GET_NTB_PARAMETERS, SET_NTB_INPUT_SIZE, packet filter, alt 1.
 * Each frame is a broadcast of ethertype 0x88B5 from the card's own address,
 * `len` bytes long.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../src/cdc.h"

static int fd;

static int control(unsigned char type, unsigned char req, unsigned short value,
                   unsigned short index, void *data, unsigned short length)
{
    struct usbdevfs_ctrltransfer c = { type, req, value, index, length, 1000, data };
    int r = ioctl(fd, USBDEVFS_CONTROL, &c);

    if (r < 0)
        fprintf(stderr, "control %02x %02x: %s\n", type, req, strerror(errno));
    return r;
}

static int set_alt(unsigned iface, unsigned alt)
{
    struct usbdevfs_setinterface s = { iface, alt };
    int r = ioctl(fd, USBDEVFS_SETINTERFACE, &s);

    if (r < 0)
        fprintf(stderr, "set_alt %u %u: %s\n", iface, alt, strerror(errno));
    return r;
}

int main(int argc, char **argv)
{
    static unsigned char ntb[32768];
    unsigned char raw[64], frame[2048];
    unsigned char mac[6] = { 0x00, 0xe0, 0x4c, 0x68, 0x01, 0x2a };
    NCM_PARAMS p;
    NCM_TX tx;
    unsigned i, n, ndp_end, len, count, skip;
    const char *dev = getenv("XE_DEV");

    if (argc < 3) {
        fprintf(stderr, "usage: %s ndp_end len [count] [skip_setup]\n", argv[0]);
        return 2;
    }
    ndp_end = atoi(argv[1]);
    len = atoi(argv[2]);
    count = argc > 3 ? atoi(argv[3]) : 3;
    skip = argc > 4 ? atoi(argv[4]) : 0;

    fd = open(dev ? dev : "/dev/bus/usb/005/004", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    for (i = 0; i < 2; i++) {
        struct usbdevfs_ioctl d = { (int)i, USBDEVFS_DISCONNECT, 0 };
        ioctl(fd, USBDEVFS_IOCTL, &d);
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i) < 0)
            fprintf(stderr, "claim %u: %s\n", i, strerror(errno));
    }

    if (!skip) {
        /* cdc_ncm_bind_common's order: toggle alt 1 -> 0 to reset the data
         * interface, read the parameters, pause, alt 1, then the input size and
         * the filter (cdc_ncm_setup, usbnet open). */
        unsigned in = 16384;
        set_alt(1, 1);
        set_alt(1, 0);
        control(0xA1, 0x80, 0, 0, raw, 28);
        usleep(15000);
        set_alt(1, 1);
        if (!getenv("XE_NOINPUT"))
            control(0x21, 0x86, 0, 0, &in, 4);
        if (!getenv("XE_NOFILTER"))
        control(0x21, 0x43, getenv("XE_FILTER") ? (unsigned short)strtoul(getenv("XE_FILTER"), 0, 0) : 0x000F, 0, 0, 0);
    }
    if (control(0xA1, 0x80, 0, 0, raw, 28) < 28 || !cdc_ncm_parse_params(raw, 28, &p)) {
        fprintf(stderr, "no NTB parameters\n");
        return 1;
    }
    printf("out_max %u divisor %u remainder %u alignment %u\n", p.out_max,
           p.out_divisor, p.out_remainder, p.out_alignment);

    /* An ARP request, who-has 10.36.0.71 tell 10.36.0.1, padded to `len`: a frame
     * every stack on the segment answers, so the far side shows it arrived. */
    memset(frame, 0, sizeof(frame));
    memset(frame, 0xff, 6);
    memcpy(frame + 6, mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;
    frame[14] = 0x00; frame[15] = 0x01; frame[16] = 0x08; frame[17] = 0x00;
    frame[18] = 6; frame[19] = 4; frame[20] = 0; frame[21] = 1;
    memcpy(frame + 22, mac, 6);
    frame[28] = 10; frame[29] = 36; frame[30] = 0; frame[31] = 123;   /* unique sender */
    frame[38] = 10; frame[39] = 36; frame[40] = 0; frame[41] = 71;
    if (len < 42)
        len = 42;

    /* Read what the device has to say first: the interrupt endpoint's link
     * notifications and whatever bulk IN holds, as usbnet starts doing the moment
     * the interface is up. XE_POLL=0 skips it. */
    if (!getenv("XE_POLL") || atoi(getenv("XE_POLL"))) {
        static unsigned char in[16384];
        unsigned char note[16];
        for (i = 0; i < 4; i++) {
            struct usbdevfs_bulktransfer b = { 0x83, sizeof(note), 200, note };
            int r = ioctl(fd, USBDEVFS_BULK, &b);
            printf("intr %d:", r);
            for (int k = 0; k < r; k++)
                printf(" %02x", note[k]);
            printf("\n");
        }
        for (i = 0; i < 4; i++) {
            struct usbdevfs_bulktransfer b = { 0x81, sizeof(in), 200, in };
            int r = ioctl(fd, USBDEVFS_BULK, &b);
            printf("bulk in %d:", r);
            for (int k = 0; k < r && k < 64; k++)
                printf(" %02x", in[k]);
            printf("\n");
        }
    }

    for (n = 0; n < count; n++) {
        struct usbdevfs_bulktransfer b;
        unsigned total;
        int r;

        cdc_ncm_tx_begin(&tx, ntb, sizeof(ntb), &p, (unsigned short)n, ndp_end);
        cdc_ncm_tx_add(&tx, frame, len);
        total = cdc_ncm_tx_finish(&tx, 512);
        if (getenv("XE_PAD")) {
            /* Pad the block out to a given size (wBlockLength covers it), as Linux
             * does for blocks past min_tx_pkt. */
            unsigned want = strtoul(getenv("XE_PAD"), 0, 0);
            memset(ntb + total, 0, want - total);
            total = want;
            ntb[8] = (unsigned char)total;
            ntb[9] = (unsigned char)(total >> 8);
        }
        if (getenv("XE_RAW")) {
            /* Send a captured NTB as it is, to tell the bytes from the setup. */
            FILE *raw_f = fopen(getenv("XE_RAW"), "rb");
            total = raw_f ? fread(ntb, 1, sizeof(ntb), raw_f) : 0;
            if (raw_f)
                fclose(raw_f);
        }
        b.ep = 0x02;
        b.len = total;
        b.timeout = 1000;
        b.data = ntb;
        r = ioctl(fd, USBDEVFS_BULK, &b);
        printf("ntb %u: %u bytes, bulk %d\n", n, total, r);
        usleep(getenv("XE_FAST") ? 200 : 100000);
    }
    return 0;
}
