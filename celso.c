/*
 * Copyright (c) 2018, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2018, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <linux/limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include "t/lib/tap.h"
#include "t/lib/testutil.h"

int celso_test(char* unifyfs_root)
{
    char path[64];
    int rc;
    struct stat sb;
    char buf[1304];

    errno = 0;

    testutil_rand_path(path, sizeof(path), unifyfs_root);

    int fd;
    fd = open(path, O_WRONLY | O_CREAT);
    ok(fd != -1, "%s: open(%s) (fd=%d): %s", __FILE__, path, fd, strerror(errno));

    rc = lseek(fd, 2144, SEEK_SET);
    ok(rc == 2144, "%s: lseek() (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = write(fd, buf, 96);
    ok(rc == 96, "%s: write() (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = lseek(fd, 0, SEEK_SET);
    ok(rc == 0, "%s: lseek() (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = write(fd, buf, 96);
    ok(rc == 96, "%s: write() (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = write(fd, buf, 1304);
    ok(rc == 1304, "%s: write() (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = fsync(fd);
    ok(rc == 0, "%s: fsync() (rc=%d): %s", __FILE__, rc, strerror(errno));
    close(fd);

    /* Laminate */
    rc = chmod(path, 0444);
    ok(rc == 0, "%s: chmod(0444) (rc=%d): %s", __FILE__, rc, strerror(errno));

    rc = stat(path, &sb);
    ok(rc == 0, "%s: stat() (rc %d): %s", __FILE__, rc, strerror(errno));
    ok(sb.st_size > 0, "%s: file size %ld > 0", __FILE__, sb.st_size);

    return 0;
}
