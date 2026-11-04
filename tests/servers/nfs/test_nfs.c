/* The NFS server on the host: built on tests/servers/common/srv.c, started on
 * a high port over a temporary directory, mounted with the PC's own NFS client
 * (sudo mount -t nfs, vers=3 over TCP), and driven with ordinary file calls on the
 * mount -- written, read, listed, renamed, truncated, removed -- each checked in the
 * exported directory itself. Skipped without mount.nfs (nfs-common) or where sudo
 * cannot mount. Built and run by
 * `make test`. */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfs.h"

#define PORT 20490

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static int run(const char *cmd)
{
    return system(cmd);
}

static long file_size(const char *p)
{
    struct stat s;

    return stat(p, &s) ? -1 : (long)s.st_size;
}

int main(void)
{
    char export_dir[] = "/tmp/xe-nfs-export-XXXXXX", mnt[] = "/tmp/xe-nfs-mnt-XXXXXX";
    char cmd[1024], p[1024], q[1024], buf[200000];
    NFS_CONFIG cfg;
    FILE *f;
    DIR *d;
    struct dirent *e;
    int i, found = 0;

    if (run("test -x /sbin/mount.nfs || test -x /usr/sbin/mount.nfs") != 0 ||
        run("sudo -n mount -V >/dev/null 2>&1") != 0 || run("sudo -n modprobe nfs 2>/dev/null") != 0) {
        printf("nfs test skipped: no NFS client here, or sudo cannot mount\n");
        return 0;
    }
    if (!mkdtemp(export_dir) || !mkdtemp(mnt)) {
        printf("FAIL: temporary directories\n");
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = export_dir;
    cfg.port = PORT;
    cfg.portmap_port = 0xFFFFFFFFu;
    CHECK(nfs_start(&cfg) == 0);
    usleep(200000);

    snprintf(cmd, sizeof(cmd),
             "sudo -n mount -t nfs -o vers=3,proto=tcp,port=%d,mountport=%d,mountproto=tcp,"
             "nolock,actimeo=0,soft,timeo=50,retrans=2 127.0.0.1:/ %s", PORT, PORT, mnt);
    if (run(cmd) != 0) {
        printf("FAIL: mount\n");
        return 1;
    }

    /* write through the mount, find it in the export */
    for (i = 0; i < (int)sizeof(buf); i++)
        buf[i] = (char)(i * 7);
    snprintf(p, sizeof(p), "%s/hello.bin", mnt);
    f = fopen(p, "wb");
    CHECK(f && fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
    if (f)
        fclose(f);
    snprintf(q, sizeof(q), "%s/hello.bin", export_dir);
    CHECK(file_size(q) == (long)sizeof(buf));

    /* read it back through a fresh mount, so the bytes come from the server and not
     * the client's cache */
    snprintf(cmd, sizeof(cmd), "sudo -n umount %s", mnt);
    CHECK(run(cmd) == 0);
    snprintf(cmd, sizeof(cmd),
             "sudo -n mount -t nfs -o vers=3,proto=tcp,port=%d,mountport=%d,mountproto=tcp,"
             "nolock,actimeo=0,soft,timeo=50,retrans=2 127.0.0.1:/ %s", PORT, PORT, mnt);
    CHECK(run(cmd) == 0);
    {
        static char back[sizeof(buf)];

        f = fopen(p, "rb");
        CHECK(f && fread(back, 1, sizeof(back), f) == sizeof(back));
        if (f)
            fclose(f);
        CHECK(!memcmp(back, buf, sizeof(buf)));
    }

    /* a directory, a file in it, a listing */
    snprintf(p, sizeof(p), "%s/dir", mnt);
    CHECK(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/dir/a.txt", mnt);
    f = fopen(p, "w");
    CHECK(f && fputs("abc", f) >= 0);
    if (f)
        fclose(f);
    snprintf(p, sizeof(p), "%s/dir", mnt);
    d = opendir(p);
    CHECK(d != 0);
    while (d && (e = readdir(d)))
        if (!strcmp(e->d_name, "a.txt"))
            found = 1;
    if (d)
        closedir(d);
    CHECK(found);

    /* rename, then the old name is gone and the new one there */
    snprintf(p, sizeof(p), "%s/dir/a.txt", mnt);
    snprintf(q, sizeof(q), "%s/dir/b.txt", mnt);
    CHECK(rename(p, q) == 0);
    snprintf(p, sizeof(p), "%s/dir/b.txt", export_dir);
    CHECK(file_size(p) == 3);
    snprintf(p, sizeof(p), "%s/dir/a.txt", export_dir);
    CHECK(file_size(p) < 0);

    /* truncate */
    snprintf(p, sizeof(p), "%s/hello.bin", mnt);
    CHECK(truncate(p, 1000) == 0);
    snprintf(q, sizeof(q), "%s/hello.bin", export_dir);
    CHECK(file_size(q) == 1000);

    /* remove the file and the directory */
    snprintf(p, sizeof(p), "%s/dir/b.txt", mnt);
    CHECK(unlink(p) == 0);
    snprintf(p, sizeof(p), "%s/dir", mnt);
    CHECK(rmdir(p) == 0);
    snprintf(p, sizeof(p), "%s/dir", export_dir);
    CHECK(file_size(p) < 0);
    snprintf(p, sizeof(p), "%s/hello.bin", mnt);
    CHECK(unlink(p) == 0);

    snprintf(cmd, sizeof(cmd), "sudo -n umount %s", mnt);
    CHECK(run(cmd) == 0);
    rmdir(mnt);
    rmdir(export_dir);
    CHECK(nfs_stats.mounts >= 1 && nfs_stats.writes > 0 && nfs_stats.reads > 0);

    if (failures)
        return 1;
    printf("all nfs tests passed (mounted with the kernel client)\n");
    return 0;
}
