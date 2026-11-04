/* srv.h's files on the console: the kernel's Nt* calls on object names, `root` an
 * object path such as \Device\Harddisk0\Partition1 and the server's '/'-paths
 * turned into '\'. The structures and argument orders are those Xenia's
 * xboxkrnl_io implements for these ordinals (21256 table): NtQueryDirectoryFile
 * takes nine arguments (handle, event, apc, context, io status, buffer, length,
 * mask, restart) and fills FILE_DIRECTORY_INFORMATION; NtSetInformationFile
 * (handle, io status, buffer, length, class) with classes 10 rename (its BOOLEAN a
 * byte at offset 0) and 20 end of file; NtDeleteFile (object attributes) removes
 * files and empty directories; NtQueryVolumeInformationFile class 3
 * FileFsSizeInformation.
 */

#include "srv.h"

extern u32 NtWriteFile(void *handle, void *event, void *apc, void *apc_context,
                       IO_STATUS_BLOCK *io, const void *buffer, u32 length, s64 *offset);
extern u32 NtDeleteFile(OBJECT_ATTRIBUTES *attributes);
extern u32 NtFlushBuffersFile(void *handle, IO_STATUS_BLOCK *io);
extern u32 NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *attributes, void *info);
extern u32 NtSetInformationFile(void *handle, IO_STATUS_BLOCK *io, void *info, u32 length,
                                u32 info_class);
extern u32 NtQueryVolumeInformationFile(void *handle, IO_STATUS_BLOCK *io, void *info,
                                        u32 length, u32 info_class);
extern u32 NtQueryDirectoryFile(void *handle, void *event, void *apc, void *apc_context,
                                IO_STATUS_BLOCK *io, void *info, u32 length,
                                ANSI_STRING *mask, u32 restart);

/* srv_sys.c: the kernel's times in seconds since 1970. */
u32 srv_nt_time(s64 t);

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
    out->atime = srv_nt_time(access);
    out->mtime = srv_nt_time(write);
    out->ctime = srv_nt_time(change);
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

/* A listing: the directory's handle, read on with NtQueryDirectoryFile one entry
 * a call (restart only on the first), and its object path for the entries' ids. */
typedef struct {
    void *handle;
    u32 started;
    u32 full_length;
    char full[FULL_MAX];
    u64 buf[1024 / 8];
} LISTING;

s32 srv_opendir(const char *root, const char *path, SRV_DIR *out)
{
    LISTING *l;
    u32 n, status;

    *out = 0;
    l = (LISTING *)srv_alloc(sizeof(LISTING));
    if (!l)
        return SRV_EIO;
    n = full_path(l->full, root, path);
    if (!n) {
        srv_free(l);
        return SRV_ENAMETOOLONG;
    }
    status = nt_open(l->full, n, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE,
                     &l->handle);
    if (!NT_SUCCESS(status)) {
        srv_free(l);
        return err(status);
    }
    if (l->full[n - 1] != '\\' && n + 1 < FULL_MAX)
        l->full[n++] = '\\';
    l->full[n] = 0;
    l->full_length = n;
    *out = l;
    return SRV_OK;
}

s32 srv_readdir(SRV_DIR dir, char *name, u32 name_size, SRV_STAT *st)
{
    LISTING *l = (LISTING *)dir;
    DIR_INFO *d = (DIR_INFO *)l->buf;
    IO_STATUS_BLOCK io;
    u32 args[10], status, i, len;

    io.Status = 0;
    args[0] = (u32)l->handle;
    args[1] = 0;
    args[2] = 0;
    args[3] = 0;
    args[4] = (u32)&io;
    args[5] = (u32)l->buf;
    args[6] = sizeof(l->buf);
    args[7] = 0;
    args[8] = !l->started;
    args[9] = (u32)&NtQueryDirectoryFile;
    status = kcall9(args);
    l->started = 1;
    if (status == STATUS_NO_MORE_FILES || status == 0xC000000Fu /* NO_SUCH_FILE */)
        return 0;
    if (!NT_SUCCESS(status))
        return err(status);
    len = d->name_length < name_size - 1 ? d->name_length : name_size - 1;
    for (i = 0; i < len; i++)
        name[i] = d->name[i];
    name[len] = 0;
    if (st) {
        for (i = 0; i < len && l->full_length + i + 1 < FULL_MAX; i++)
            l->full[l->full_length + i] = name[i];
        l->full[l->full_length + i] = 0;
        fill(st, d->attributes, d->end_of_file, d->access, d->write, d->change, l->full);
        l->full[l->full_length] = 0;
    }
    return 1;
}

void srv_closedir(SRV_DIR dir)
{
    LISTING *l = (LISTING *)dir;

    if (l) {
        NtClose(l->handle);
        srv_free(l);
    }
}

s32 srv_flush(SRV_FILE f)
{
    IO_STATUS_BLOCK io;
    u32 status;

    io.Status = 0;
    status = NtFlushBuffersFile(f, &io);
    return NT_SUCCESS(status) ? SRV_OK : err(status);
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
