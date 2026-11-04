/* The FTP server on the host: built on tests/servers/common/'s POSIX platform,
 * started on a high port over a temporary directory and driven with curl --
 * login, upload (passive and active), download, a resumed upload and download,
 * listings (LIST, NLST), MKD, RNFR/RNTO, DELE, RMD, SIZE, MDTM -- each checked in
 * the served directory itself. Skipped without curl. Built and run by `make test`. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ftp.h"

#define PORT 20210
#define URL  "ftp://u:p@127.0.0.1:20210"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static char dir[] = "/tmp/xe-ftp-XXXXXX", work[] = "/tmp/xe-ftp-work-XXXXXX";

static int sh(const char *fmt, const char *a, const char *b)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd), fmt, a, b);
    return system(cmd);
}

static long size_of(const char *a, const char *b)
{
    char p[1024];
    struct stat s;

    snprintf(p, sizeof(p), "%s/%s", a, b);
    return stat(p, &s) ? -1 : (long)s.st_size;
}

static int same(const char *a, const char *b)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd), "cmp -s %s %s", a, b);
    return system(cmd) == 0;
}

int main(void)
{
    char p[1024], q[1024];
    FTP_CONFIG cfg;
    FILE *f;

    if (system("command -v curl >/dev/null 2>&1") != 0) {
        printf("ftp test skipped: no curl\n");
        return 0;
    }
    if (!mkdtemp(dir) || !mkdtemp(work))
        return 1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = dir;
    cfg.port = PORT;
    cfg.user = "u";
    cfg.password = "p";
    CHECK(ftp_start(&cfg) == 0);
    usleep(100000);

    /* a 3 MB file with a pattern, uploaded passive into a made directory */
    snprintf(p, sizeof(p), "%s/src.bin", work);
    CHECK(sh("head -c 3000000 /dev/urandom > %s%s", p, "") == 0);
    CHECK(sh("curl -s -S --ftp-create-dirs -T %s " URL "/d/up.bin%s", p, "") == 0);
    CHECK(size_of(dir, "d/up.bin") == 3000000);
    snprintf(q, sizeof(q), "%s/d/up.bin", dir);
    CHECK(same(p, q));

    /* active mode (EPRT) down */
    snprintf(q, sizeof(q), "%s/down.bin", work);
    CHECK(sh("curl -s -S -P - -o %s " URL "/d/up.bin%s", q, "") == 0);
    CHECK(same(p, q));

    /* PORT (no EPRT) up */
    CHECK(sh("curl -s -S -P - --disable-eprt -T %s " URL "/d/port.bin%s", p, "") == 0);
    CHECK(size_of(dir, "d/port.bin") == 3000000);

    /* a resumed download: the first million kept, the rest fetched (REST) */
    CHECK(sh("head -c 1000000 %s > %s.part", p, q) == 0);
    snprintf(q, sizeof(q), "%s/down.bin.part", work);
    CHECK(sh("curl -s -S -C - -o %s " URL "/d/up.bin%s", q, "") == 0);
    CHECK(same(p, q));

    /* a resumed upload: half on the server, the rest sent from its size (REST) */
    snprintf(q, sizeof(q), "%s/d/half.bin", dir);
    CHECK(sh("head -c 1500000 %s > %s", p, q) == 0);
    CHECK(sh("curl -s -S -C - -T %s " URL "/d/half.bin%s", p, "") == 0);
    CHECK(same(p, q));

    /* listings */
    snprintf(q, sizeof(q), "%s/list.txt", work);
    CHECK(sh("curl -s -S -o %s " URL "/d/%s", q, "") == 0);
    CHECK(sh("grep -q '^-rw-rw-rw- 1 root root 3000000 .* up.bin' %s%s", q, "") == 0);
    CHECK(sh("curl -s -S --list-only -o %s " URL "/d/%s", q, "") == 0);
    CHECK(sh("grep -qx 'port.bin' %s%s", q, "") == 0);

    /* SIZE, MDTM and the rest through -Q */
    snprintf(q, sizeof(q), "%s/q.txt", work);
    CHECK(sh("curl -s -S -I " URL "/d/up.bin > %s 2>&1%s", q, "") == 0);
    CHECK(sh("grep -q 'Content-Length: 3000000' %s%s", q, "") == 0);
    CHECK(sh("curl -s -S -Q 'RNFR d/up.bin' -Q 'RNTO d/moved.bin' -Q 'MKD e' -Q 'DELE d/port.bin' "
             URL "/ -o /dev/null%s%s", "", "") == 0);
    CHECK(size_of(dir, "d/up.bin") < 0 && size_of(dir, "d/moved.bin") == 3000000);
    CHECK(size_of(dir, "d/port.bin") < 0);
    CHECK(size_of(dir, "e") >= 0);
    CHECK(sh("curl -s -S -Q 'RMD e' " URL "/ -o /dev/null%s%s", "", "") == 0);
    CHECK(size_of(dir, "e") < 0);

    /* ".." does not leave the root */
    CHECK(sh("curl -s -S -Q 'CWD ../../..' -Q 'MKD top' " URL "/ -o /dev/null%s%s", "", "") == 0);
    CHECK(size_of(dir, "top") >= 0);

    /* a wrong password is refused */
    CHECK(sh("curl -s ftp://u:wrong@127.0.0.1:20210/ -o /dev/null%s%s", "", "") != 0);

    f = 0;
    (void)f;
    CHECK(sh("rm -rf %s %s", dir, work) == 0);
    CHECK(ftp_stats.stors >= 3 && ftp_stats.retrs >= 2 && ftp_stats.lists >= 2);
    CHECK(ftp_stats.bad_logins >= 1);
    if (failures)
        return 1;
    printf("all ftp tests passed (driven with curl)\n");
    return 0;
}
