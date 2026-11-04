/* Servers that keep files open, and what closes them: FATX does not reliably
 * remove or rename a file another handle has open (some went at the last close,
 * some stayed, 2026-11-04), so the platform's srv_unlink, srv_rmdir, srv_rename,
 * srv_truncate and a truncating srv_open ask every keeper to let go of the name
 * first -- whichever server calls them. Pure code: the console's platform and the
 * host tests' both use it. */

#include "srv.h"

#define KEEPERS 4

static void (*keepers[KEEPERS])(const char *root, const char *path);

void srv_keeper(void (*forget)(const char *root, const char *path))
{
    u32 i;

    for (i = 0; i < KEEPERS; i++)
        if (!keepers[i] || keepers[i] == forget) {
            keepers[i] = forget;
            return;
        }
}

void srv_forget(const char *root, const char *path)
{
    u32 i;

    for (i = 0; i < KEEPERS && keepers[i]; i++)
        keepers[i](root, path);
}
