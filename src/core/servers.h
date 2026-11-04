/* Where the core lets the file servers in (src/servers/), and nothing more.
 *
 * The servers are built only when named: `make servers=nfs`. Without any, these
 * are empty inlines and the plugin carries none of their code (nor xam's sockets,
 * which only they and bench=tcp import).
 */

#ifndef XE_SERVERS_H
#define XE_SERVERS_H

#include "kernel.h"

#ifndef XE_SERVERS
#define XE_SERVERS 0
#endif

#if XE_SERVERS
/* Every worker tick (20 ms): starts the servers once the console's network is up. */
void servers_tick(u32 tick);
#else
static inline void servers_tick(u32 tick) { (void)tick; }
#endif

#endif /* XE_SERVERS_H */
