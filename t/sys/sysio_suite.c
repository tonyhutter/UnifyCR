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

#include <string.h>
#include <mpi.h>
#include <unifyfs.h>
#include "t/lib/tap.h"
#include "t/lib/testutil.h"

#include "sysio_suite.h"

/* The test suite for sysio wrappers found in client/src/unifyfs-sysio.c.
 *
 *
 * To add new tests to existing sysio tests:
 * 1. Simply add the tests (order matters) to the appropriate
 *    <sysio_function_name>.c file.
 *
 *
 * When a new wrapper in unifyfs-sysio.c needs to be tested:
 * 1. Create a <sysio_function_name>.c file with a function called
 *    <sysio_function_name>_test(char* unifyfs_root) that contains all the TAP
 *    tests specific to that wrapper.
 * 2. Add the <sysio_function_name>_test to sysio_suite.h.
 * 3. Add the <sysio_function_name>.c file to the /t/Makefile.am under the
 *    appropriate test suite at the bottom.
 * 4. The <sysio_function_name>_test function can now be called from this test
 *    suite. */
int main(int argc, char* argv[])
{
    int rank_num;
    int rank;
    char* unifyfs_root;
    int rc;

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &rank_num);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    plan(NO_PLAN);

    unifyfs_root = testutil_get_mount_point();

    /* Verify unifyfs_mount succeeds. */
    rc = unifyfs_mount(unifyfs_root, rank, rank_num, 0);
    ok(rc == 0, "unifyfs_mount at %s (rc=%d)", unifyfs_root, rc);

    /* If the mount fails, bailout, as there is no point in running the tests */
    if (rc != 0) {
        BAIL_OUT("unifyfs_mount in sysio_suite failed");
    }

    /* Add tests for new functions below in the order desired for testing.
     *
     * *** NOTE ***
     * The order of the tests does matter as some subsequent tests use
     * functions that were already tested (i.e., mkdir_rmdir_test uses the
     * creat function). Thus if creat fails, it could cause later tests to
     * fail. If this occurs, fix the bugs causing the tests that ran first to
     * break as that is likely to cause subsequent failures to start passing.
     */

    creat_close_test(unifyfs_root);

    creat64_test(unifyfs_root);

    mkdir_rmdir_test(unifyfs_root);

    open_test(unifyfs_root);

    open64_test(unifyfs_root);

    celso_test(unifyfs_root);

    MPI_Finalize();

    done_testing();

    return 0;
}
