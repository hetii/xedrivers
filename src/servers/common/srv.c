/* srv.h on the console: xam's sockets, the kernel's threads, pool and files.
 *
 * Sockets: xam's NetDll_* by ordinal, as the bench's TCP server calls them (caller
 * kind 2, a system app); every socket gets xnet's two insecure-socket options or
 * xnet drops its SYNs (bench/tcp/tcp.c). Addresses and ports in a sockaddr are
 * already in network order on this big-endian console.
 *
 * Files: the kernel's Nt* calls on object names, `root` an object path such as
 * \Device\Harddisk0\Partition1 and the server's '/'-paths turned into '\'. The
 * structures and argument orders are those Xenia's xboxkrnl_io implements for these
 * ordinals (21256 table): NtQueryDirectoryFile takes nine arguments (handle, event,
 * apc, context, io status, buffer, length, mask, restart) and fills
 * FILE_DIRECTORY_INFORMATION; NtSetInformationFile (handle, io status, buffer,
 * length, class) with classes 10 rename (its BOOLEAN a byte at offset 0) and 20 end
 * of file; NtDeleteFile (object attributes) removes files and empty directories;
 * NtQueryVolumeInformationFile class 3 FileFsSizeInformation.
 *
 * Threads: ExCreateThread on a trampoline that first sets the priority data threads
 * need (20, as bench/tcp measured: below 19 xnet's own threads take the quantum).
 */

#include "srv.h"
#include "file.h"

/* --- the kernel and xam ------------------------------------------------------- */

extern void *ExAllocatePoolTypeWithTag(u32 bytes, u32 tag, u32 pool_type);
extern u32 KeSetPriorityThread(void *thread, u32 priority);
extern u32 KeSetAffinityThread(void *thread, u32 affinity, u32 *previous);
extern void KeQuerySystemTime(s64 *time);
extern u32 NtWriteFile(void *handle, void *event, void *apc, void *apc_context,
                       IO_STATUS_BLOCK *io, const void *buffer, u32 length, s64 *offset);
extern u32 NtDeleteFile(OBJECT_ATTRIBUTES *attributes);
extern u32 NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *attributes, void *info);
extern u32 NtSetInformationFile(void *handle, IO_STATUS_BLOCK *io, void *info, u32 length,
                                u32 info_class);
extern u32 NtQueryVolumeInformationFile(void *handle, IO_STATUS_BLOCK *io, void *info,
                                        u32 length, u32 info_class);
extern u32 NtQueryDirectoryFile(void *handle, void *event, void *apc, void *apc_context,
                                IO_STATUS_BLOCK *io, void *info, u32 length,
                                ANSI_STRING *mask, u32 restart);

extern int NetDll_WSAStartup(u32 caller, u16 version, void *data);
extern u32 NetDll_socket(u32 caller, int family, int type, int protocol);
extern int NetDll_closesocket(u32 caller, u32 socket);
extern int NetDll_setsockopt(u32 caller, u32 socket, int level, int option,
                             const void *value, int length);
extern int NetDll_getsockname(u32 caller, u32 socket, void *address, int *length);
extern int NetDll_bind(u32 caller, u32 socket, const void *address, int length);
extern int NetDll_connect(u32 caller, u32 socket, const void *address, int length);
extern int NetDll_listen(u32 caller, u32 socket, int backlog);
extern u32 NetDll_accept(u32 caller, u32 socket, void *address, int *length);
extern int NetDll_recv(u32 caller, u32 socket, void *buffer, int length, int flags);
extern int NetDll_send(u32 caller, u32 socket, const void *buffer, int length, int flags);
extern int NetDll_recvfrom(u32 caller, u32 socket, void *buffer, int length, int flags,
                           void *from, int *from_length);
extern int NetDll_sendto(u32 caller, u32 socket, const void *buffer, int length, int flags,
                         const void *to, int to_length);

#define CALLER_SYSAPP   2
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define SOL_SOCKET      0xFFFF
#define SO_REUSEADDR    0x0004
#define SO_XNET_5802    0x5802
#define SO_XNET_5801    0x5801
#define INVALID_SOCKET  0xFFFFFFFFu

#define GENERIC_READ        0x80000000u
#define GENERIC_WRITE       0x40000000u
#define DELETE_ACCESS       0x00010000u
#define SYNCHRONIZE         0x00100000u
#define FILE_LIST_DIRECTORY 0x00000001u
#define FILE_SHARE_ALL      0x00000007u
#define FILE_OPEN           1u
#define FILE_CREATE         2u
#define FILE_OPEN_IF        3u
#define FILE_OVERWRITE_IF   5u
#define FILE_DIRECTORY_FILE 0x00000001u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define FILE_NON_DIRECTORY_FILE 0x00000040u
#define OBJ_CASE_INSENSITIVE 0x00000040u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u

#define STATUS_NO_MORE_FILES        0x80000006u

typedef struct {
    u16 family;
    u16 port;
    u32 address;
    u8  zero[8];
} SOCKADDR_IN;

/* NtQueryFullAttributesFile's FILE_NETWORK_OPEN_INFORMATION. */
typedef struct {
    s64 creation, access, write, change, allocation, end_of_file;
    u32 attributes;
    u32 pad;
} NET_OPEN_INFO;

/* NtQueryDirectoryFile's FILE_DIRECTORY_INFORMATION. */
typedef struct {
    u32 next;
    u32 index;
    s64 creation, access, write, change, end_of_file, allocation;
    u32 attributes;
    u32 name_length;
    char name[1];
} DIR_INFO;

static volatile u32 started;

static void start_once(void)
{
    static u32 wsa[128];

    if (!started) {
        NetDll_WSAStartup(CALLER_SYSAPP, 0x0202, wsa);
        started = 1;
    }
}

/* --- sockets ----------------------------------------------------------------- */

static u32 insecure(u32 s)
{
    u32 one = 1;

    return NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_XNET_5802, &one, 4) >= 0 &&
           NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_XNET_5801, &one, 4) >= 0;
}

static SRV_SOCK bound(int type, int protocol, u32 port)
{
    SOCKADDR_IN a;
    u32 s, i, one = 1;

    start_once();
    s = NetDll_socket(CALLER_SYSAPP, AF_INET, type, protocol);
    if (s == INVALID_SOCKET)
        return -1;
    if (!insecure(s)) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_REUSEADDR, &one, 4);
    a.family = AF_INET;
    a.port = (u16)port;
    a.address = 0;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    if (NetDll_bind(CALLER_SYSAPP, s, &a, sizeof(a)) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    return (SRV_SOCK)s;
}

SRV_SOCK srv_listen(u32 port, u32 backlog)
{
    SRV_SOCK s = bound(SOCK_STREAM, IPPROTO_TCP, port);

    if (s >= 0 && NetDll_listen(CALLER_SYSAPP, (u32)s, (int)backlog) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, (u32)s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_udp(u32 port)
{
    return bound(SOCK_DGRAM, IPPROTO_UDP, port);
}

SRV_SOCK srv_accept(SRV_SOCK listener, u32 *peer)
{
    SOCKADDR_IN a;
    int n = sizeof(a);
    u32 s = NetDll_accept(CALLER_SYSAPP, (u32)listener, &a, &n);

    if (s == INVALID_SOCKET)
        return -1;
    insecure(s);
    if (peer)
        *peer = a.address;
    return (SRV_SOCK)s;
}

SRV_SOCK srv_connect(u32 address, u32 port)
{
    SOCKADDR_IN a;
    u32 s, i;

    start_once();
    s = NetDll_socket(CALLER_SYSAPP, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return -1;
    insecure(s);
    a.family = AF_INET;
    a.port = (u16)port;
    a.address = address;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    if (NetDll_connect(CALLER_SYSAPP, s, &a, sizeof(a)) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    return (SRV_SOCK)s;
}

s32 srv_recv(SRV_SOCK s, void *buffer, u32 length)
{
    int n = NetDll_recv(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0);

    return n < 0 ? -1 : n;
}

s32 srv_send(SRV_SOCK s, const void *buffer, u32 length)
{
    const u8 *p = (const u8 *)buffer;
    u32 done = 0;
    int n;

    while (done < length) {
        n = NetDll_send(CALLER_SYSAPP, (u32)s, p + done, (int)(length - done), 0);
        if (n <= 0)
            return -1;
        done += (u32)n;
    }
    return (s32)done;
}

s32 srv_recvfrom(SRV_SOCK s, void *buffer, u32 length, u32 *address, u32 *port)
{
    SOCKADDR_IN a;
    int n = sizeof(a), r = NetDll_recvfrom(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0, &a, &n);

    if (r < 0)
        return -1;
    *address = a.address;
    *port = a.port;
    return r;
}

s32 srv_sendto(SRV_SOCK s, const void *buffer, u32 length, u32 address, u32 port)
{
    SOCKADDR_IN a;
    u32 i;

    a.family = AF_INET;
    a.port = (u16)port;
    a.address = address;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    return NetDll_sendto(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0, &a, sizeof(a)) < 0
               ? -1 : (s32)length;
}

void srv_close(SRV_SOCK s)
{
    if (s >= 0)
        NetDll_closesocket(CALLER_SYSAPP, (u32)s);
}

u32 srv_local_address(SRV_SOCK s)
{
    SOCKADDR_IN a;
    int n = sizeof(a);

    return NetDll_getsockname(CALLER_SYSAPP, (u32)s, &a, &n) < 0 ? 0 : a.address;
}

u32 srv_local_port(SRV_SOCK s)
{
    SOCKADDR_IN a;
    int n = sizeof(a);

    return NetDll_getsockname(CALLER_SYSAPP, (u32)s, &a, &n) < 0 ? 0 : a.port;
}

/* --- threads ----------------------------------------------------------------- */

#define THREAD_PRIORITY 20
#define THREAD_STACK    0x10000u
#define STARTS          32

/* The hardware thread the servers run on (srv_cpu in the ini, 4 by default): not
 * 0 or 1 (a server pinned there hung the console once, bench/tcp), not 2 (the USB
 * stack's DPCs) and not 5 (the driver's worker). Which one serves best is still
 * to be measured. */
static u32 thread_cpu = 0xFFFFFFFFu;

typedef struct {
    void (*fn)(void *);
    void *arg;
    volatile u32 used;
} START;

static START starts[STARTS];

static inline void *current_thread(void)
{
    void *t;
#if defined(__powerpc__)
    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(t));
#else
    t = 0;
#endif
    return t;
}

static u32 trampoline(void *p)
{
    START *s = (START *)p;
    void (*fn)(void *) = s->fn;
    void *arg = s->arg;
    u32 previous;

    mem_sync();
    s->used = 0;
    KeSetAffinityThread(current_thread(), 1u << thread_cpu, &previous);
    KeSetPriorityThread(current_thread(), THREAD_PRIORITY);
    fn(arg);
    return 0;
}

s32 srv_thread(void (*fn)(void *arg), void *arg)
{
    void *thread = 0;
    u32 i;
    START *s = 0;

    srv_lock();
    for (i = 0; i < STARTS; i++)
        if (!starts[i].used) {
            s = &starts[i];
            s->used = 1;
            break;
        }
    srv_unlock();
    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    mem_sync();
    if (thread_cpu == 0xFFFFFFFFu) {
        u32 cpu = srv_setting_u32("srv_cpu", 4);

        thread_cpu = cpu >= 2 && cpu <= 5 ? cpu : 4;
    }
    if (ExCreateThread(&thread, THREAD_STACK, 0, 0, (void *)trampoline, s, 2) < 0) {
        s->used = 0;
        return -1;
    }
    NtClose(thread);
    return 0;
}

void srv_sleep(u32 ms)
{
    s64 interval = -((s64)ms * 10000);      /* relative, 100 ns units */

    KeDelayExecutionThread(0, 0, &interval);
}

/* --- memory and the lock ------------------------------------------------------ */

void *srv_alloc(u32 bytes)
{
    u8 *p = (u8 *)ExAllocatePoolTypeWithTag(bytes, 0x76727378u /* "xsrv" */, 2);
    u32 i;

    if (p)
        for (i = 0; i < bytes; i++)
            p[i] = 0;
    return p;
}

static KSPIN_LOCK lock;
static u32 lock_irql;

void srv_lock(void)
{
    u32 irql = KfAcquireSpinLock(&lock);

    lock_irql = irql;
}

void srv_unlock(void)
{
    KfReleaseSpinLock(&lock, lock_irql);
}

/* --- files ------------------------------------------------------------------- */

#define FULL_MAX 512

/* root + "\" + path with '/' turned into '\'; 0 if too long. */
static u32 full_path(char *out, const char *root, const char *path)
{
    u32 n = 0;

    while (*root && n + 1 < FULL_MAX)
        out[n++] = *root++;
    while (*path == '/')
        path++;
    /* always a '\\' after the root: without one, a root such as
     * \Device\Harddisk0\Partition1 names the volume, not its top directory */
    if ((!n || out[n - 1] != '\\') && n + 1 < FULL_MAX)
        out[n++] = '\\';
    for (; *path; path++) {
        if (n + 1 >= FULL_MAX)
            return 0;
        out[n++] = *path == '/' ? '\\' : *path;
    }
    out[n] = 0;
    return n;
}

static void attributes_of(OBJECT_ATTRIBUTES *a, ANSI_STRING *name, const char *path, u32 n)
{
    name->Length = (u16)n;
    name->MaximumLength = (u16)(n + 1);
    name->Buffer = path;
    a->RootDirectory = 0;
    a->ObjectName = name;
    a->Attributes = OBJ_CASE_INSENSITIVE;
}

static s32 err(u32 status)
{
    switch (status) {
    case 0xC0000034u:                       /* OBJECT_NAME_NOT_FOUND */
    case 0xC000003Au:                       /* OBJECT_PATH_NOT_FOUND */
    case 0xC000000Fu:                       /* NO_SUCH_FILE */
        return SRV_ENOENT;
    case 0xC0000035u:                       /* OBJECT_NAME_COLLISION */
        return SRV_EEXIST;
    case 0xC0000022u:                       /* ACCESS_DENIED */
    case 0xC0000043u:                       /* SHARING_VIOLATION */
        return SRV_EACCES;
    case 0xC0000103u:                       /* NOT_A_DIRECTORY */
        return SRV_ENOTDIR;
    case 0xC00000BAu:                       /* FILE_IS_A_DIRECTORY */
        return SRV_EISDIR;
    case 0xC000007Fu:                       /* DISK_FULL */
        return SRV_ENOSPC;
    case 0xC0000101u:                       /* DIRECTORY_NOT_EMPTY */
        return SRV_ENOTEMPTY;
    case 0xC0000033u:                       /* OBJECT_NAME_INVALID */
    case 0xC000000Du:                       /* INVALID_PARAMETER */
        return SRV_EINVAL;
    default:
        return SRV_EIO;
    }
}

static u32 nt_open(const char *full, u32 n, u32 access, u32 disposition, u32 options,
                   void **handle)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES a;
    IO_STATUS_BLOCK io;
    u32 args[10];

    attributes_of(&a, &name, full, n);
    io.Status = 0;
    *handle = 0;
    args[0] = (u32)handle;
    args[1] = access | SYNCHRONIZE;
    args[2] = (u32)&a;
    args[3] = (u32)&io;
    args[4] = 0;
    args[5] = 0;
    args[6] = FILE_SHARE_ALL;
    args[7] = disposition;
    args[8] = options | FILE_SYNCHRONOUS_IO_NONALERT;
    args[9] = (u32)&NtCreateFile;
    return kcall9(args);
}

/* 100 ns since 1601 into seconds since 1970. */
static u32 unix_time(s64 t)
{
    u64 x, q = 0, d = 10000000ull << 31;
    u32 i;

    if (t <= 116444736000000000LL)
        return 0;
    x = (u64)(t - 116444736000000000LL);
    for (i = 0; i < 32; i++, d >>= 1) {         /* no 64-bit divide in the plugin */
        q <<= 1;
        if (x >= d) {
            x -= d;
            q |= 1;
        }
    }
    return (u32)q;
}

s32 srv_open(const char *root, const char *path, u32 mode, SRV_FILE *out)
{
    char full[FULL_MAX];
    u32 n = full_path(full, root, path), status, access = 0, disposition = FILE_OPEN;
    void *h;

    if (!n)
        return SRV_ENAMETOOLONG;
    if (mode & SRV_READ)
        access |= GENERIC_READ;
    if (mode & SRV_WRITE)
        access |= GENERIC_WRITE;
    if (mode & SRV_CREATE)
        disposition = (mode & SRV_EXCL) ? FILE_CREATE :
                      (mode & SRV_TRUNC) ? FILE_OVERWRITE_IF : FILE_OPEN_IF;
    status = nt_open(full, n, access, disposition, FILE_NON_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status))
        return err(status);
    *out = h;
    return SRV_OK;
}

s32 srv_read(SRV_FILE f, u32 offset_lo, u32 offset_hi, void *buffer, u32 length)
{
    IO_STATUS_BLOCK io;
    s64 at = (s64)((u64)offset_hi << 32 | offset_lo);
    u32 status;

    io.Status = 0;
    io.Information = 0;
    status = NtReadFile(f, 0, 0, 0, &io, buffer, length, &at);
    if (status == 0xC0000011u)              /* END_OF_FILE */
        return 0;
    return NT_SUCCESS(status) ? (s32)io.Information : err(status);
}

s32 srv_write(SRV_FILE f, u32 offset_lo, u32 offset_hi, const void *buffer, u32 length)
{
    IO_STATUS_BLOCK io;
    s64 at = (s64)((u64)offset_hi << 32 | offset_lo);
    u32 status;

    io.Status = 0;
    io.Information = 0;
    status = NtWriteFile(f, 0, 0, 0, &io, buffer, length, &at);
    return NT_SUCCESS(status) ? (s32)io.Information : err(status);
}

void srv_fclose(SRV_FILE f)
{
    if (f)
        NtClose(f);
}

static void fill(SRV_STAT *out, u32 attributes, s64 eof, s64 access, s64 write, s64 change,
                 const char *full)
{
    u32 h = 2166136261u;

    out->is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size_lo = (u32)eof;
    out->size_hi = (u32)((u64)eof >> 32);
    out->atime = unix_time(access);
    out->mtime = unix_time(write);
    out->ctime = unix_time(change);
    while (*full)                           /* FATX has no inode numbers: the path's */
        h = (h ^ (u8)(*full >= 'a' && *full <= 'z' ? *full - 32 : *full)) * 16777619u,
        full++;
    out->id = h | 1u;
}

s32 srv_stat(const char *root, const char *path, SRV_STAT *out)
{
    char full[FULL_MAX];
    ANSI_STRING name;
    OBJECT_ATTRIBUTES a;
    NET_OPEN_INFO info;
    u32 n = full_path(full, root, path), status;

    if (!n)
        return SRV_ENAMETOOLONG;
    attributes_of(&a, &name, full, n);
    status = NtQueryFullAttributesFile(&a, &info);
    if (!NT_SUCCESS(status))
        return err(status);
    fill(out, info.attributes, info.end_of_file, info.access, info.write, info.change, full);
    return SRV_OK;
}

static s32 set_info(const char *root, const char *path, u32 access, u32 options,
                    void *info, u32 length, u32 info_class)
{
    char full[FULL_MAX];
    IO_STATUS_BLOCK io;
    u32 n = full_path(full, root, path), status;
    void *h;

    if (!n)
        return SRV_ENAMETOOLONG;
    status = nt_open(full, n, access, FILE_OPEN, options, &h);
    if (!NT_SUCCESS(status))
        return err(status);
    io.Status = 0;
    status = NtSetInformationFile(h, &io, info, length, info_class);
    NtClose(h);
    return NT_SUCCESS(status) ? SRV_OK : err(status);
}

s32 srv_truncate(const char *root, const char *path, u32 size_lo, u32 size_hi)
{
    s64 size = (s64)((u64)size_hi << 32 | size_lo);

    return set_info(root, path, GENERIC_WRITE, FILE_NON_DIRECTORY_FILE, &size, 8, 20);
}

s32 srv_mkdir(const char *root, const char *path)
{
    char full[FULL_MAX];
    u32 n = full_path(full, root, path), status;
    void *h;

    if (!n)
        return SRV_ENAMETOOLONG;
    status = nt_open(full, n, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status))
        return err(status);
    NtClose(h);
    return SRV_OK;
}

/* NtDeleteFile on the name; the kind checked first, as POSIX callers expect. */
static s32 nt_delete(const char *root, const char *path, u32 want_dir)
{
    char full[FULL_MAX];
    ANSI_STRING name;
    OBJECT_ATTRIBUTES a;
    SRV_STAT st;
    u32 n = full_path(full, root, path), status;
    s32 e;

    if (!n)
        return SRV_ENAMETOOLONG;
    e = srv_stat(root, path, &st);
    if (e != SRV_OK)
        return e;
    if (st.is_dir != want_dir)
        return want_dir ? SRV_ENOTDIR : SRV_EISDIR;
    attributes_of(&a, &name, full, n);
    status = NtDeleteFile(&a);
    return NT_SUCCESS(status) ? SRV_OK : err(status);
}

s32 srv_rmdir(const char *root, const char *path)
{
    return nt_delete(root, path, 1);
}

s32 srv_unlink(const char *root, const char *path)
{
    return nt_delete(root, path, 0);
}

/* FileRenameInformation: replace-if-exists, a root directory handle (none) and the
 * new name as a full object path. */
s32 srv_rename(const char *root, const char *from, const char *to)
{
    struct {
        u8 replace;                         /* BOOLEAN */
        u8 pad[3];
        void *root_dir;
        ANSI_STRING name;
    } info;
    char full_to[FULL_MAX];
    u32 n = full_path(full_to, root, to);

    if (!n)
        return SRV_ENAMETOOLONG;
    info.replace = 1;
    info.pad[0] = info.pad[1] = info.pad[2] = 0;
    info.root_dir = 0;
    info.name.Length = (u16)n;
    info.name.MaximumLength = (u16)(n + 1);
    info.name.Buffer = full_to;
    return set_info(root, from, DELETE_ACCESS, 0, &info, sizeof(info), 10);
}

/* NtQueryDirectoryFile one entry at a time, restarted at the start and walked to
 * the cookie: simple, and directories here are small. */
s32 srv_readdir(const char *root, const char *path, u32 *cookie, char *name,
                u32 name_size, SRV_STAT *st)
{
    char full[FULL_MAX];
    u64 buf_store[1024 / 8];
    u8 *buf = (u8 *)buf_store;
    IO_STATUS_BLOCK io;
    u32 n = full_path(full, root, path), status, index = 0, i, len;
    void *h;
    s32 result = 0;

    if (!n)
        return SRV_ENAMETOOLONG;
    status = nt_open(full, n, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status))
        return err(status);
    for (;;) {
        DIR_INFO *d = (DIR_INFO *)buf;
        u32 args[10];

        io.Status = 0;
        args[0] = (u32)h;
        args[1] = 0;
        args[2] = 0;
        args[3] = 0;
        args[4] = (u32)&io;
        args[5] = (u32)buf;
        args[6] = 1024;
        args[7] = 0;
        args[8] = index == 0;
        args[9] = (u32)&NtQueryDirectoryFile;
        status = kcall9(args);
        if (!NT_SUCCESS(status) || status == STATUS_NO_MORE_FILES)
            break;
        if (index++ < *cookie)
            continue;
        len = d->name_length < name_size - 1 ? d->name_length : name_size - 1;
        for (i = 0; i < len; i++)
            name[i] = d->name[i];
        name[len] = 0;
        if (st) {
            char child_full[FULL_MAX];
            u32 k = 0;

            for (i = 0; full[i] && k + 1 < FULL_MAX; i++)
                child_full[k++] = full[i];
            if (k && child_full[k - 1] != '\\' && k + 1 < FULL_MAX)
                child_full[k++] = '\\';
            for (i = 0; i < len && k + 1 < FULL_MAX; i++)
                child_full[k++] = name[i];
            child_full[k] = 0;
            fill(st, d->attributes, d->end_of_file, d->access, d->write, d->change,
                 child_full);
        }
        *cookie = index;
        result = 1;
        break;
    }
    NtClose(h);
    return result;
}

s32 srv_volume(const char *root, u32 *total_lo, u32 *total_hi, u32 *free_lo, u32 *free_hi)
{
    char full[FULL_MAX];
    IO_STATUS_BLOCK io;
    struct {
        s64 total_units;
        s64 free_units;
        u32 sectors_per_unit;
        u32 bytes_per_sector;
    } info;
    u32 n = full_path(full, root, ""), status;
    u64 unit, t, f;
    void *h;

    status = nt_open(full, n, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status))
        return err(status);
    io.Status = 0;
    status = NtQueryVolumeInformationFile(h, &io, &info, sizeof(info), 3);
    NtClose(h);
    if (!NT_SUCCESS(status))
        return err(status);
    unit = (u64)info.sectors_per_unit * info.bytes_per_sector;
    t = (u64)info.total_units * unit;
    f = (u64)info.free_units * unit;
    *total_lo = (u32)t;
    *total_hi = (u32)(t >> 32);
    *free_lo = (u32)f;
    *free_hi = (u32)(f >> 32);
    return SRV_OK;
}

/* --- time and settings -------------------------------------------------------- */

u32 srv_now(void)
{
    s64 t = 0;

    KeQuerySystemTime(&t);
    return unix_time(t);
}

#define SETTINGS "xedrivers.ini"            /* beside the plugin (src/core/file.h) */

u32 srv_setting(const char *key, char *out, u32 size)
{
    return file_setting(SETTINGS, key, out, size) != 0;
}

u32 srv_setting_u32(const char *key, u32 fallback)
{
    char v[24];
    u32 i, n = 0, base = 10;

    if (!srv_setting(key, v, sizeof(v)))
        return fallback;
    i = 0;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        i = 2;
    }
    for (; v[i]; i++) {
        u32 d = v[i] >= '0' && v[i] <= '9' ? (u32)(v[i] - '0') :
                v[i] >= 'a' && v[i] <= 'f' ? (u32)(v[i] - 'a' + 10) :
                v[i] >= 'A' && v[i] <= 'F' ? (u32)(v[i] - 'A' + 10) : 99;

        if (d >= base)
            break;
        n = n * base + d;
    }
    return n;
}
