/* Reading files: xedrivers.ini and the drivers' firmware, beside the plugin.
 *
 * The plugin lives in a directory of its own, e.g. Usb:\plugin\xedrivers\xedrivers.xex,
 * with xedrivers.ini beside it and what the drivers read from disk under firmware\, a
 * directory a driver named as in src/drivers/: firmware\rtw8822b\rtw8822b_fw.bin,
 * firmware\aic8800\... (the vendors' files, as Linux loads them; uncompressed --
 * distributions ship some as .zst), and data of ours built from src/, such as
 * firmware\rtw8822b\rtw8822b_tables.bin. Only what is read once, at a card's start.
 *
 * Paths are kernel object names ("\\Device\\Mass0\\plugin\\xedrivers\\x.bin"). A file is
 * looked for in every directory of file_dirs, in order: the plugin's own directory
 * first (from its loader entry), then the places the plugin is usually put.
 * Thread-only (PASSIVE_LEVEL).
 */

#ifndef XE_FILE_H
#define XE_FILE_H

#include "kernel.h"

/* What a driver reads from disk lives under this, in a directory named as the driver. */
#define FIRMWARE_DIR "firmware\\"

/* Learn the plugin's own directory from DllMain's `instance`, its loader entry. */
void file_set_plugin(void *instance);

/* Open `name` in the first of the directories that has it. Returns the handle, or 0;
 * file_stats records what was tried. */
void *file_open(const char *name);

/* Read up to `length` bytes at `offset`. Returns the bytes read, 0 at the end or on
 * an error. */
u32 file_read(void *handle, void *buffer, u32 length, u32 offset);

void file_close(void *handle);

/* Write `name` anew with `data`: where it is now (the first directory that has it), else
 * beside the plugin. 0 if written whole. Thread-only. */
u32 file_replace(const char *name, const void *data, u32 length);

/* One setting of a small "key=value" file (xedrivers.ini beside the plugin): the
 * value of `key`, up to `size` - 1 bytes and terminated, without the spaces around
 * it. Lines starting with '#' or ';' are comments. Returns its length, or 0 when
 * the file or the key is not there. Thread-only. */
u32 file_setting(const char *name, const char *key, char *out, u32 size);

typedef struct {
    u32  opens;
    u32  found;
    u32  last_status;               /* of the last NtCreateFile tried */
    u32  read_errors;
    u32  last_read_status;
    char plugin_dir[96];            /* the plugin's directory, "" if not known */
    char last_path[96];             /* the last path opened, or the last tried */
    u32  last_write_status;
} FILE_STATS;

extern volatile FILE_STATS file_stats;

#endif /* XE_FILE_H */
