/* lwIP as the console's own network stack (src/stack): IPv4, TCP with window scaling
 * and SACK, UDP, ARP, ICMP, DHCP, and the socket API on threads of the kernel. */
#ifndef XE_LWIPOPTS_H
#define XE_LWIPOPTS_H

/* --- the system ------------------------------------------------------------- */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_COMPAT_MUTEX               1       /* a mutex is a semaphore of one */
#define LWIP_COMPAT_MUTEX_ALLOWED       1
#define LWIP_TCPIP_CORE_LOCKING         1       /* the caller's thread does the work, under one lock */
#define LWIP_TCPIP_CORE_LOCKING_INPUT   1       /* and so does the thread that brings frames in */
#define TCPIP_THREAD_STACKSIZE          0x8000
#define TCPIP_THREAD_PRIO               20
#define TCPIP_MBOX_SIZE                 64
#define DEFAULT_THREAD_STACKSIZE        0x8000
#define DEFAULT_THREAD_PRIO             20
#define LWIP_NETCONN_SEM_PER_THREAD     0

/* --- memory: all of it the plugin's own image, none taken at run time ------- */
#define MEM_ALIGNMENT                   8       /* the semaphores inside lwIP's objects */
#define MEM_SIZE                        (256 * 1024)
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                24
#define MEMP_NUM_TCP_PCB_LISTEN         12
#define MEMP_NUM_TCP_SEG                512
#define MEMP_NUM_NETBUF                 32
#define MEMP_NUM_NETCONN                40
#define MEMP_NUM_TCPIP_MSG_API          16
#define MEMP_NUM_TCPIP_MSG_INPKT        64
#define MEMP_NUM_SYS_TIMEOUT            16
#define PBUF_POOL_SIZE                  512     /* a received frame each: 800 KB */
#define PBUF_POOL_BUFSIZE               1536

/* --- protocols -------------------------------------------------------------- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_ACD                        0
#define LWIP_AUTOIP                     0
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0
#define ARP_TABLE_SIZE                  16
#define ARP_QUEUEING                    1
#define ETH_PAD_SIZE                    0

/* TCP: a receive window xnet cannot have (its pool), scaled; out-of-order segments
 * kept and told to the sender with SACK, so one loss costs one segment, not a
 * timeout. */
#define TCP_MSS                         1460
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   3
#define TCP_WND                         (384 * 1024)
#define TCP_SND_BUF                     (128 * 1024)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_SNDLOWAT                    32768   /* a 16-bit field in lwIP */
#define TCP_QUEUE_OOSEQ                 1
#define TCP_OOSEQ_MAX_PBUFS             300
#define LWIP_TCP_SACK_OUT               1
#define LWIP_TCP_MAX_SACK_NUM           4
#define LWIP_TCP_TIMESTAMPS             0
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16
#define LWIP_TCP_KEEPALIVE              1
#define TCP_OVERSIZE                    TCP_MSS

/* --- the socket API --------------------------------------------------------- */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             0       /* lwip_socket(), not socket(): xam has those names */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_SOCKET_SELECT              1
#define LWIP_SOCKET_POLL                0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define RECV_BUFSIZE_DEFAULT            TCP_WND
#define SO_REUSE                        1
#define LWIP_NETIF_API                  0
#define DEFAULT_TCP_RECVMBOX_SIZE       320     /* a segment each: the window in segments, and some */
#define DEFAULT_UDP_RECVMBOX_SIZE       32
#define DEFAULT_ACCEPTMBOX_SIZE         16
#define LWIP_TIMEVAL_PRIVATE            1

/* --- checks and statistics -------------------------------------------------- */
#define LWIP_STATS                      0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_SINGLE_NETIF               0
#define LWIP_NOASSERT                   0
#define LWIP_DEBUG                      0

#endif
