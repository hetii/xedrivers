# xeftp: an FTP server

Directory: `src/servers/xeftp/` · built with `make servers=xeftp` · port 21 (`ftp_port`)

FTP (RFC 959) with SIZE, MDTM, REST STREAM and MLST/MLSD (RFC 3659), and EPSV/EPRT
(RFC 2428). It uses only the platform of `src/servers/common/srv.h`.

## Files

| File | What |
|---|---|
| `ftp.h` | the API: `ftp_start`, the config, the counters |
| `ftpd.h` | the internals every file shares |
| `ftp.c` | start, sessions, the command loop, replies, paths |
| `commands.c` | login, directories, names, file information |
| `transfer.c` | PASV/EPSV/PORT/EPRT, RETR/STOR/APPE with REST, LIST/NLST/MLSD |

## How it works

- **Sessions**: one thread a session, 8 at most.
  - A session closes after 10 minutes without a command.
  - A passive connection the client never opens is given up after 30 s, with 425.
  - A STOR with nothing arriving for 60 s ends.
- **Paths**: `/` is the root (every volume by default, as ftpdll shows them); `..`
  stops there.
- **Data**: every TYPE is sent as it is. A transfer moves 64 KB a call. A STOR is
  flushed before its 226.
- **Login**: checked only when `ftp_user` is set. Otherwise any login is taken, as
  with ftpdll on a LAN.

## Settings (`xedrivers.ini`)

| Key | Default | What |
|---|---|---|
| `ftp` | 1 | 0 turns the server off |
| `ftp_root` | `*` | every volume, or one object path |
| `ftp_port` | 21 | another server holding the port is waited for (2 minutes) |
| `ftp_user`, `ftp_pass` | none | the login |
| `ftp_allow` | anyone | `a.b.c.d/n`: the clients let in |
| `ftp_pasv_low`, `ftp_pasv_count` | 50000, 1000 | the passive ports, taken in turn (xnet's own are only 1025-1255) |

With ftpdll loaded as well, give this one another port (`ftp_port = 2121`).

## Measured (2026-11-04, RTL8156B, ECM, 512 MiB, curl)

| | this server | ftpdll, same boot |
|---|---|---|
| PC -> console | 213 Mbit/s (26.6 MB/s) | 165 Mbit/s (20.7 MB/s) |
| console -> PC | 185 Mbit/s (23.1 MB/s) | 166 Mbit/s (20.8 MB/s) |

A build with servers has 32 TCP sockets: a session holds up to three (control, passive
listener, data), and on xnet's 16 four sessions at once found none. 8 at once on the
RTL8822BU at 5 GHz: 204 Mbit/s (25.5 MB/s) in, 179 Mbit/s (22.4 MB/s) out.

## Tests

`tests/servers/xeftp/test_ftp.c` builds the server on the host and drives it with
curl:
- transfers in passive, EPRT and PORT mode;
- a resumed upload and a resumed download;
- LIST, NLST and SIZE;
- RNFR/RNTO, MKD, RMD and DELE;
- `..` at the root, and a wrong password.

Skipped without curl.
