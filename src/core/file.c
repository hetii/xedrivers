/* Reading files; see file.h. */

#include "file.h"

#define GENERIC_READ                 0x80000000u
#define SYNCHRONIZE                  0x00100000u
#define FILE_SHARE_READ              0x00000001u
#define FILE_OPEN                    0x00000001u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define FILE_NON_DIRECTORY_FILE      0x00000040u
#define OBJ_CASE_INSENSITIVE         0x00000040u

volatile FILE_STATS file_stats;

/* Where else a plugin and its files are usually put: the first USB drives and the
 * hard disk, each with the directory DashLaunch's examples use. */
static const char *const fallback_dirs[] = {
    "\\Device\\Mass0\\plugin",
    "\\Device\\Mass1\\plugin",
    "\\Device\\Mass2\\plugin",
    "\\Device\\Harddisk0\\Partition1\\plugin",
};

static char plugin_dir[96];

static u32 copy(char *to, u32 at, u32 size, const char *from)
{
    while (*from && at + 1 < size)
        to[at++] = *from++;
    to[at] = 0;
    return at;
}

static void note(volatile char *to, const char *from)
{
    u32 i;

    for (i = 0; from[i] && i + 1 < 96; i++)
        to[i] = from[i];
    to[i] = 0;
}

/* The loader entry: FullDllName, a UNICODE_STRING (length in bytes, maximum,
 * buffer of big-endian UTF-16), at +0x24 after three list links and the image's
 * base, entry and size. Only the directory is kept, and only if it is an object
 * name (starts with a backslash) of plain ASCII. */
void file_set_plugin(void *instance)
{
    const u8 *entry = (const u8 *)instance;
    const u16 *text;
    u32 length, i, last = 0;

    if (!entry || !MmIsAddressValid(entry))
        return;
    length = (u32)(entry[0x24] << 8 | entry[0x25]) / 2;
    text = *(const u16 *const *)(entry + 0x28);
    if (!length || length >= sizeof(plugin_dir) || !text || !MmIsAddressValid(text))
        return;
    for (i = 0; i < length; i++) {
        if (!text[i] || text[i] > 0x7E)
            return;
        plugin_dir[i] = (char)text[i];
        if (plugin_dir[i] == '\\')
            last = i;
    }
    plugin_dir[last] = 0;
    note(file_stats.plugin_dir, plugin_dir);
    if (plugin_dir[0] != '\\')
        plugin_dir[0] = 0;
}

static void *open_path(const char *path)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    void *handle = 0;
    u32 args[10], length = 0, status;

    while (path[length])
        length++;
    name.Length = (u16)length;
    name.MaximumLength = (u16)(length + 1);
    name.Buffer = path;
    attributes.RootDirectory = 0;
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    io.Status = 0;
    args[0] = (u32)&handle;
    args[1] = GENERIC_READ | SYNCHRONIZE;
    args[2] = (u32)&attributes;
    args[3] = (u32)&io;
    args[4] = 0;
    args[5] = 0;
    args[6] = FILE_SHARE_READ;
    args[7] = FILE_OPEN;
    args[8] = FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE;
    args[9] = (u32)&NtCreateFile;
    status = kcall9(args);
    file_stats.last_status = status;
    note(file_stats.last_path, path);
    return NT_SUCCESS(status) ? handle : 0;
}

void *file_open(const char *name)
{
    char path[160];
    void *handle;
    u32 i, at;

    file_stats.opens++;
    for (i = 0; i <= sizeof(fallback_dirs) / sizeof(fallback_dirs[0]); i++) {
        const char *dir = i ? fallback_dirs[i - 1] : plugin_dir;

        if (!dir[0])
            continue;
        at = copy(path, 0, sizeof(path), dir);
        at = copy(path, at, sizeof(path), "\\");
        copy(path, at, sizeof(path), name);
        handle = open_path(path);
        if (handle) {
            file_stats.found++;
            return handle;
        }
    }
    return 0;
}

u32 file_read(void *handle, void *buffer, u32 length, u32 offset)
{
    IO_STATUS_BLOCK io;
    s64 at = offset;
    u32 status;

    io.Status = 0;
    io.Information = 0;
    status = NtReadFile(handle, 0, 0, 0, &io, buffer, length, &at);
    if (!NT_SUCCESS(status)) {
        if (status != 0xC0000011u) {            /* STATUS_END_OF_FILE */
            file_stats.read_errors++;
            file_stats.last_read_status = status;
        }
        return 0;
    }
    return io.Information;
}

void file_close(void *handle)
{
    if (handle)
        NtClose(handle);
}

#define SETTINGS_MAX 2048

static char settings[SETTINGS_MAX];

static u32 is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

u32 file_setting(const char *name, const char *key, char *out, u32 size)
{
    void *h = file_open(name);
    u32 length = 0, got, at = 0, k, end, n;

    if (size)
        out[0] = 0;
    if (!h)
        return 0;
    do {
        got = file_read(h, settings + length, SETTINGS_MAX - 1 - length, length);
        length += got;
    } while (got && length < SETTINGS_MAX - 1);
    file_close(h);
    settings[length] = 0;

    while (at < length) {
        end = at;
        while (end < length && settings[end] != '\n')
            end++;
        while (at < end && is_space(settings[at]))
            at++;
        if (at < end && settings[at] != '#' && settings[at] != ';') {
            for (k = 0; key[k] && at + k < end && settings[at + k] == key[k]; k++)
                ;
            if (!key[k]) {
                u32 v = at + k;

                while (v < end && is_space(settings[v]))
                    v++;
                if (v < end && settings[v] == '=') {
                    v++;
                    while (v < end && is_space(settings[v]))
                        v++;
                    n = end;
                    while (n > v && is_space(settings[n - 1]))
                        n--;
                    for (k = 0; v + k < n && k + 1 < size; k++)
                        out[k] = settings[v + k];
                    out[k] = 0;
                    return k;
                }
            }
        }
        at = end + 1;
    }
    return 0;
}
