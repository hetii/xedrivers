/* The console's disk alone: how fast files are written and read back with no network
 * in the way (bench=disk, `tools/xectl.py bench disk`). */
#ifndef XE_BENCH_DISK_H
#define XE_BENCH_DISK_H

#include "kernel.h"

#define DISK_FILES_MAX  8u

typedef struct {
    u32 state;          /* 0 idle, 1 writing, 2 reading, 3 done */
    u32 status;         /* the first NTSTATUS that was not success, or 0 */
    u32 files;          /* written at once, a thread each */
    u32 file_kb;        /* each file */
    u32 chunk_kb;       /* one NtWriteFile / NtReadFile */
    u32 write_ms;       /* first write to the last file flushed and closed; these three in
                         * units of 0.8192 ms (the system time >> 13) */
    u32 read_ms;        /* first read to the last */
    u32 slowest_ms;     /* the longest single write */
    u32 written_kb;
    u32 read_kb;
} DISK_STATS;

extern volatile DISK_STATS disk_stats;

/* `files` files of `mb` megabytes in all, in chunks of `chunk_kb`, on the hard disk's
 * root; read back, then deleted. 0 when started. */
u32 disk_start(u32 mb, u32 chunk_kb, u32 files);

#endif
