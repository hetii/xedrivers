/* Native test of the one tricky pure function in the standalone stack: the IPv4
 * header checksum. Run by `make test`; no console needed. The rest of stack.c
 * touches net_tx/usb and is exercised on hardware. */

#include <stdio.h>

#include "../src/stack.h"
#include "../src/net.h"
#include "../src/usb.h"

/* stack.c links against the driver; the checksum test reaches none of it, so these
 * stand in for the symbols the linker still wants. */
u8 net_mac[ETH_ALEN];
volatile NET_RX net_rx;
volatile NET_LINK net_link_cb;
volatile NET_STATS net_stats;
volatile USB_STATS usb_stats;
USB_DEV usb;
u32 net_tx(const u8 *frame, u32 length) { (void)frame; (void)length; return 0; }
u32 net_link_up(void) { return 0; }
u32 net_tx_space(void) { return 0; }
volatile NET_TX_IDLE net_tx_idle;
u32 net_speed_mbps(void) { return 0; }
void *net_dma_alloc(u32 bytes) { (void)bytes; return 0; }
void usb_sleep(u32 ms) { (void)ms; }

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void)
{
    /* A real IPv4 header (RFC 1071 worked example style): checksum field zero,
     * computed value is well known for this classic header. */
    unsigned char hdr[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01,
        0xc0, 0xa8, 0x00, 0xc7
    };
    unsigned short got = ip_checksum(hdr, 20, 0);

    CHECK(got == 0xb861);

    /* Putting the checksum back in makes the whole header sum to zero (~0). */
    hdr[10] = (unsigned char)(got >> 8);
    hdr[11] = (unsigned char)(got & 0xFF);
    CHECK(ip_checksum(hdr, 20, 0) == 0);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all stack tests passed\n");
    return 0;
}
