/* Host test for src/net/txq.c: frames leave in order, and a card that says it has room but
 * refuses every frame does not keep the drainer going round for ever (2026-11-10: an AIC8800
 * whose handshake gate refused data while its ring had free slots -- the thread that handed
 * xnet's frame in never came back, and the console hung). */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "txq.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                  failures++; } } while (0)

u32 KfAcquireSpinLock(KSPIN_LOCK *lock) { (void)lock; return 0; }
void KfReleaseSpinLock(KSPIN_LOCK *lock, u32 irql) { (void)lock; (void)irql; }

static u32 refuse, room = 1, sends, completed, last_sent;

static u32 t_send(void *o, const u8 *f, u32 n)
{
    (void)o; (void)n;
    sends++;
    if (refuse)
        return 0;
    last_sent = f[0];
    return 1;
}
static u32 t_space(void *o) { (void)o; return room; }
static u32 t_keep(void *o) { (void)o; return 1; }
static void t_done(void *o, void *u, void *c) { (void)o; (void)u; (void)c; completed++; }

static NET_TXQ q = { .send = t_send, .space = t_space, .keep = t_keep, .done = t_done };

static void hung(int sig)
{
    (void)sig;
    printf("FAIL: the drain never returned (a refused frame with room left)\n");
    exit(1);
}

int main(void)
{
    static u8 frames[4][64];
    u32 i, length, type;

    signal(SIGALRM, hung);
    for (i = 0; i < 4; i++)
        frames[i][0] = (u8)i;

    net_txq_put(&q, 0, frames[0], 64, 0);           /* goes at once */
    CHECK(completed == 1 && last_sent == 0);

    /* room said, every frame refused: put must come back, the frames kept in order */
    refuse = 1;
    alarm(2);
    net_txq_put(&q, 0, frames[1], 64, 0);
    net_txq_put(&q, 0, frames[2], 64, 0);
    alarm(0);
    CHECK(completed == 1 && q.head != q.tail);

    /* the card takes frames again: the next drain sends them, in order */
    refuse = 0;
    net_txq_drain(&q);
    CHECK(completed == 3 && last_sent == 2 && q.head == q.tail);

    /* a frame refused for good is what the stall watch drops (xnet.c) */
    refuse = 1;
    net_txq_put(&q, 0, frames[3], 64, 0);
    CHECK(net_txq_drop_head(&q, &length, &type) == 1 && length == 64 && completed == 4);

    if (!failures)
        printf("all txq tests passed\n");
    return failures != 0;
}
