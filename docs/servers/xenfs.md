# xenfs: an NFSv3 server

Directory: `src/servers/xenfs/` · built with `make servers=xenfs` · ports 2049 (NFS and
MOUNT, TCP and UDP) and 111 (portmapper and rpcbind)

NFS v3 (RFC 1813) over TCP, with MOUNT v1-v3 and a portmapper v2 / rpcbind v3-v4
beside it, so a client finds every port itself. It uses only the platform of
`src/servers/common/srv.h`, never the drivers or xnet's internals.

## Mounting

```sh
sudo mount -t nfs -o vers=3,nolock 192.168.1.210:/ /mnt/xbox
sudo mount -t nfs -o vers=3,nolock,nconnect=4 192.168.1.210:/ /mnt/xbox   # 4 connections
```

`/` holds the volumes there are: `Hdd1`, `Usb0`-`Usb2`, `IntUsb`, `Mu0`, `Mu1`.
Mount `hard` (the default): a `soft` mount with a short `timeo` drops writes still
queued in the client and does not tell the writer.

## Files

| File | What |
|---|---|
| `nfs.h` | the API: `nfs_start`, the config, the counters |
| `nfsd.h` | the internals every file shares |
| `nfs.c` | start, sockets (TCP record marking, datagrams), RPC, XDR |
| `portmap.c` | the portmapper and rpcbind, MOUNT |
| `handle.c` | file handles, attributes |
| `nfs3.c` | NFS v3 on files and the file system, the open files |
| `nfs3_dir.c` | NFS v3 on directories |

## How it works

- **Connections**: one thread a TCP connection, 16 at most; one closes after 15
  minutes without a call.
- **Handles**: 8 bytes, a node's index and generation. The nodes are a tree of
  names (a parent and a name), 16384 by default (`nfs_handles`, 80 B each). A full
  table gives up a leaf no client used for a while. A rename moves one node and
  everything below follows. A handle from before a restart is stale, never another
  file. The fileid is the node's, 64 bits: unique on a disk of millions of files,
  and the same across a rename.
- **Listings**: a connection keeps the directory it lists open and reads on from
  the cookie where the last answer ended. A listing never restarts from the top.
- **Open files**: one table for every connection, closed after 5 s unused. A
  remove, rename or truncation closes the name everywhere first, whichever server
  asks (`srv_keeper`). FATX does not reliably delete a file another handle holds.
- **Writes**: UNSTABLE as the client asks it, flushed by COMMIT
  (`NtFlushBuffersFile`). The write verifier changes at every start, so a client
  sends again what a restart lost.
- **Access**: AUTH_SYS is taken as given; everything is readable and writable.
  `nfs_allow` limits the clients by address.

## Settings (`xedrivers.ini`)

| Key | Default | What |
|---|---|---|
| `nfs` | 1 | 0 turns the server off |
| `nfs_root` | `*` | every volume, or one object path (`\Device\Harddisk0\Partition1`) |
| `nfs_allow` | anyone | `a.b.c.d/n`: the clients let in |
| `nfs_handles` | 16384 | names clients may hold handles to |
| `srv_cpu` | 4 | hardware thread of the servers' threads (2-5) |
| `srv_delay` | 30 | seconds after boot before the servers start |

## Measured (2026-11-04, RTL8156B, ECM, 512 MiB)

| | 1 connection | `nconnect=4` | `nconnect=8` |
|---|---|---|---|
| PC -> console | 207 Mbit/s (25.9 MB/s) | 210 Mbit/s (26.2 MB/s) | 209 Mbit/s (26.1 MB/s) |
| console -> PC | 182 Mbit/s (22.8 MB/s) | 174 Mbit/s (21.8 MB/s) | |

Both are the card's own TCP ceilings (in 214 Mbit/s, out 175 Mbit/s); the
hardware thread (`srv_cpu` 2-5) changes nothing. On the RTL8822BU at 5 GHz:
167-191 Mbit/s (20.9-23.9 MB/s) in, 168-174 Mbit/s (21.0-21.8 MB/s) out.
`ls -l` of a directory of 4000 files: 0.1 s.

## Tests

`tests/servers/xenfs/test_nfs.c` builds the server on the host and mounts it with
the PC's own NFS client. It runs write, read after a remount, a directory, a
listing, rename, truncate and remove. It lists 3000 names through a table of 256
handles, and renames a file while it is open. Skipped without `mount.nfs` or
without sudo for mount.
