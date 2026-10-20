/* Reading files: a driver's firmware, beside the plugin.
 *
 * Paths are kernel object names ("\\Device\\Mass0\\plugin\\x.bin"). A firmware file
 * is looked for in every directory of file_dirs, in order: the plugin's own
 * directory first (from its loader entry), then the places a plugin is usually put.
 * Thread-only (PASSIVE_LEVEL).
 */

#ifndef XE_FILE_H
#define XE_FILE_H

#include "kernel.h"

/* Learn the plugin's own directory from DllMain's `instance`, its loader entry. */
void file_set_plugin(void *instance);

/* Open `name` in the first of the directories that has it. Returns the handle, or 0;
 * file_stats records what was tried. */
void *file_open(const char *name);

/* Read up to `length` bytes at `offset`. Returns the bytes read, 0 at the end or on
 * an error. */
u32 file_read(void *handle, void *buffer, u32 length, u32 offset);

void file_close(void *handle);

typedef struct {
    u32  opens;
    u32  found;
    u32  last_status;               /* of the last NtCreateFile tried */
    u32  read_errors;
    u32  last_read_status;
    char plugin_dir[96];            /* the plugin's directory, "" if not known */
    char last_path[96];             /* the last path opened, or the last tried */
} FILE_STATS;

extern volatile FILE_STATS file_stats;

#endif /* XE_FILE_H */
