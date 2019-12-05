/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Weikuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICENSE for full license text.
 */

/*
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * code Written by
 *   Raghunath Rajachandrasekar <rajachan@cse.ohio-state.edu>
 *   Kathryn Mohror <kathryn@llnl.gov>
 *   Adam Moody <moody20@llnl.gov>
 * All rights reserved.
 * This file is part of CRUISE.
 * For details, see https://github.com/hpc/cruise
 * Please also read this file LICENSE.CRUISE
 */

#include "unifyfs-internal.h"
#include "unifyfs-sysio.h"
#include "margo_client.h"
#include "ucr_read_builder.h"

/* -------------------
 * define external variables
 * --------------------*/

extern int unifyfs_spilloverblock;
extern int unifyfs_use_spillover;

#define MAX(a, b) (a > b ? a : b)

/* ---------------------------------------
 * POSIX wrappers: paths
 * --------------------------------------- */

int UNIFYFS_WRAP(access)(const char* path, int mode)
{
    /* determine whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* check if path exists */
        if (unifyfs_get_fid_from_path(path) < 0) {
            LOGDBG("access: unifyfs_get_id_from path failed, returning -1, %s",
                   path);
            errno = ENOENT;
            return -1;
        }

        /* currently a no-op */
        LOGDBG("access: path intercepted, returning 0, %s", path);
        return 0;
    } else {
        LOGDBG("access: calling MAP_OR_FAIL, %s", path);
        MAP_OR_FAIL(access);
        int ret = UNIFYFS_REAL(access)(path, mode);
        LOGDBG("access: returning __real_access %d, %s", ret, path);
        return ret;
    }
}

int UNIFYFS_WRAP(mkdir)(const char* path, mode_t mode)
{
    /* Support for directories is very limited at this time
     * mkdir simply puts an entry into the filelist for the
     * requested directory (assuming it does not exist)
     * It doesn't check to see if parent directory exists */

    /* determine whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* check if it already exists */
        if (unifyfs_get_fid_from_path(path) >= 0) {
            errno = EEXIST;
            return -1;
        }

        /* add directory to file list */
        int ret = unifyfs_fid_create_directory(path);
        if (ret != UNIFYFS_SUCCESS) {
            /* failed to create the directory,
             * set errno and return */
            errno = unifyfs_err_map_to_errno(ret);
            return -1;
        }

        /* success */
        return 0;
    } else {
        MAP_OR_FAIL(mkdir);
        int ret = UNIFYFS_REAL(mkdir)(path, mode);
        return ret;
    }
}

int UNIFYFS_WRAP(rmdir)(const char* path)
{
    /* determine whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* check if the mount point itself is being deleted */
        if (!strcmp(path, unifyfs_mount_prefix)) {
            errno = EBUSY;
            return -1;
        }

        /* check if path exists */
        int fid = unifyfs_get_fid_from_path(path);
        if (fid < 0) {
            errno = ENOENT;
            return -1;
        }

        /* is it a directory? */
        if (!unifyfs_fid_is_dir(fid)) {
            errno = ENOTDIR;
            return -1;
        }

        /* is it empty? */
        if (!unifyfs_fid_is_dir_empty(path)) {
            errno = ENOTEMPTY;
            return -1;
        }

        /* remove the directory from the file list */
        int ret = unifyfs_fid_unlink(fid);
        if (ret != UNIFYFS_SUCCESS) {
            /* failed to remove the directory,
             * set errno and return */
            errno = unifyfs_err_map_to_errno(ret);
            return -1;
        }

        /* success */
        return 0;
    } else {
        MAP_OR_FAIL(rmdir);
        int ret = UNIFYFS_REAL(rmdir)(path);
        return ret;
    }
}

int UNIFYFS_WRAP(rename)(const char* oldpath, const char* newpath)
{
    /* TODO: allow oldpath / newpath to split across memfs and normal
     * linux fs, which means we'll need to do a read / write */

    /* check whether the old path is in our file system */
    if (unifyfs_intercept_path(oldpath)) {
        /* for now, we can only rename within our file system */
        if (!unifyfs_intercept_path(newpath)) {
            /* ERROR: can't yet rename across file systems */
            errno = EXDEV;
            return -1;
        }

        /* verify that we really have a file by the old name */
        int fid = unifyfs_get_fid_from_path(oldpath);
        if (fid < 0) {
            /* ERROR: oldname does not exist */
            LOGDBG("Couldn't find entry for %s in UNIFYFS", oldpath);
            errno = ENOENT;
            return -1;
        }
        LOGDBG("orig file in position %d", fid);

        /* check that new name is within bounds */
        size_t newpathlen = strlen(newpath) + 1;
        if (newpathlen > UNIFYFS_MAX_FILENAME) {
            errno = ENAMETOOLONG;
            return -1;
        }

        /* TODO: rename should replace existing file atomically */

        /* verify that we don't already have a file by the new name */
        int newfid = unifyfs_get_fid_from_path(newpath);
        if (newfid >= 0) {
            /* something exists in newpath, need to delete it */
            int ret = UNIFYFS_WRAP(unlink)(newpath);
            if (ret == -1) {
                /* failed to unlink */
                errno = EBUSY;
                return -1;
            }
        }

        /* finally overwrite the old name with the new name */
        LOGDBG("Changing %s to %s",
               (char*)&unifyfs_filelist[fid].filename, newpath);
        strcpy((void*)&unifyfs_filelist[fid].filename, newpath);

        /* success */
        return 0;
    } else {
        /* for now, we can only rename within our file system */
        if (unifyfs_intercept_path(newpath)) {
            /* ERROR: can't yet rename across file systems */
            errno = EXDEV;
            return -1;
        }

        /* both files are normal linux files, delegate to system call */
        MAP_OR_FAIL(rename);
        int ret = UNIFYFS_REAL(rename)(oldpath, newpath);
        return ret;
    }
}

int UNIFYFS_WRAP(truncate)(const char* path, off_t length)
{
    /* determine whether we should intercept this path or not */
    if (unifyfs_intercept_path(path)) {
        /* lookup the fid for the path */
        int fid = unifyfs_get_fid_from_path(path);
        if (fid < 0) {
            /* ERROR: file does not exist */
            LOGDBG("Couldn't find entry for %s in UNIFYFS", path);
            errno = ENOENT;
            return -1;
        }

        /* truncate the file */
        int rc = unifyfs_fid_truncate(fid, length);
        if (rc != UNIFYFS_SUCCESS) {
            LOGDBG("unifyfs_fid_truncate failed for %s in UNIFYFS", path);
            errno = EIO;
            return -1;
        }

        /* success */
        return 0;
    } else {
        MAP_OR_FAIL(truncate);
        int ret = UNIFYFS_REAL(truncate)(path, length);
        return ret;
    }
}

int UNIFYFS_WRAP(unlink)(const char* path)
{
    /* determine whether we should intercept this path or not */
    if (unifyfs_intercept_path(path)) {
        /* get file id for path name */
        int fid = unifyfs_get_fid_from_path(path);
        if (fid < 0) {
            /* ERROR: file does not exist */
            LOGDBG("Couldn't find entry for %s in UNIFYFS", path);
            errno = ENOENT;
            return -1;
        }

        /* check that it's not a directory */
        if (unifyfs_fid_is_dir(fid)) {
            /* ERROR: is a directory */
            LOGDBG("Attempting to unlink a directory %s in UNIFYFS", path);
            errno = EISDIR;
            return -1;
        }

        /* delete the file */
        int ret = unifyfs_fid_unlink(fid);
        if (ret != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(ret);
            return -1;
        }

        /* success */
        return 0;
    } else {
        MAP_OR_FAIL(unlink);
        int ret = UNIFYFS_REAL(unlink)(path);
        return ret;
    }
}

int UNIFYFS_WRAP(remove)(const char* path)
{
    /* determine whether we should intercept this path or not */
    if (unifyfs_intercept_path(path)) {
        /* get file id for path name */
        int fid = unifyfs_get_fid_from_path(path);
        if (fid < 0) {
            /* ERROR: file does not exist */
            LOGDBG("Couldn't find entry for %s in UNIFYFS", path);
            errno = ENOENT;
            return -1;
        }

        /* check that it's not a directory */
        if (unifyfs_fid_is_dir(fid)) {
            /* TODO: shall be equivalent to rmdir(path) */
            /* ERROR: is a directory */
            LOGDBG("Attempting to remove a directory %s in UNIFYFS", path);
            errno = EISDIR;
            return -1;
        }

        /* shall be equivalent to unlink(path) */
        /* delete the file */
        int ret = unifyfs_fid_unlink(fid);
        if (ret != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(ret);
            return -1;
        }

        /* success */
        return 0;
    } else {
        MAP_OR_FAIL(remove);
        int ret = UNIFYFS_REAL(remove)(path);
        return ret;
    }
}

/* The main stat call for all the *stat() functions */
static int __stat(const char* path, struct stat* buf)
{
    int gfid, fid;
    unifyfs_file_attr_t fattr;
    int ret;

    gfid = unifyfs_generate_gfid(path);
    fid = unifyfs_get_fid_from_path(path);

    /* check that caller gave us a buffer to write to */
    if (!buf) {
        errno = EFAULT;
        return -1;
    }

    /* lookup stat data for global file id */
    ret = invoke_client_metaget_rpc(gfid, &fattr);
    if (ret != UNIFYFS_SUCCESS) {
        LOGDBG("metaget failed");
        return ret;
    }

    memset(buf, 0, sizeof(*buf));

    /* copy attributes to stat struct */
    unifyfs_file_attr_to_stat(&fattr, buf);

    /*
     * For debugging and testing purposes, we hijack st_rdev to store our
     * local size and log size.  We also assume the stat struct is
     * the 64-bit variant.  The values are stored as:
     *
     * st_rdev = log_size << 32 | local_size;
     *
     */
    buf->st_rdev = fid;
    if (fid >= 0) { /* If we have a local file */
        buf->st_rdev = (unifyfs_fid_log_size(fid) << 32) |
            (unifyfs_fid_local_size(fid) & 0xFFFFFFFF);
    }

    if (!fattr.is_laminated) {
        /*
         * It was decided that all non-laminated files would report a global
         * filesize of zero.
         */
        buf->st_size = 0;
    }

    return 0;
}

int UNIFYFS_WRAP(stat)(const char* path, struct stat* buf)
{
    LOGDBG("stat was called for %s", path);
    if (unifyfs_intercept_path(path)) {
        return __stat(path, buf);
    } else {
        MAP_OR_FAIL(stat);
        return UNIFYFS_REAL(stat)(path, buf);
    }
}

int UNIFYFS_WRAP(fstat)(int fd, struct stat* buf)
{
    int fid;
    const char* path;
    LOGDBG("fstat was called for fd: %d", fd);

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        fid = unifyfs_get_fid_from_fd(fd);
        path = unifyfs_path_from_fid(fid);
        return __stat(path, buf);
    } else {
        MAP_OR_FAIL(fstat);
        return UNIFYFS_REAL(fstat)(fd, buf);
    }
}

/*
 * NOTE on __xstat(2), __lxstat(2), and __fxstat(2)
 * The additional parameter vers shall be 3 or the behavior of these functions
 * is undefined. (ISO POSIX(2003))
 *
 * from /sys/stat.h, it seems that we need to test if vers being _STAT_VER,
 * instead of using the absolute value 3.
 */

#ifdef HAVE___XSTAT
int UNIFYFS_WRAP(__xstat)(int vers, const char* path, struct stat* buf)
{
    LOGDBG("xstat was called for %s", path);

    if (unifyfs_intercept_path(path)) {
        if (vers != _STAT_VER) {
            errno = EINVAL;
            return -1;
        }
        return __stat(path, buf);
    } else {
        MAP_OR_FAIL(__xstat);
        int ret = UNIFYFS_REAL(__xstat)(vers, path, buf);
        return ret;
    }
}
#endif

#ifdef HAVE___LXSTAT
int UNIFYFS_WRAP(__lxstat)(int vers, const char* path, struct stat* buf)
{
    LOGDBG("lxstat was called for %s", path);

    if (unifyfs_intercept_path(path)) {
        if (vers != _STAT_VER) {
            errno = EINVAL;
            return -1;
        }
        return __stat(path, buf);
    } else {
        MAP_OR_FAIL(__lxstat);
        return UNIFYFS_REAL(__lxstat)(vers, path, buf);
    }
}
#endif

#ifdef HAVE___FXSTAT
int UNIFYFS_WRAP(__fxstat)(int vers, int fd, struct stat* buf)
{
    LOGDBG("fxstat was called for fd %d", fd);
    int fid;
    const char* path;

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        if (vers != _STAT_VER) {
            errno = EINVAL;
            return -1;
        }

        fid = unifyfs_get_fid_from_fd(fd);
        path = unifyfs_path_from_fid(fid);
        return __stat(path, buf);
    } else {
        MAP_OR_FAIL(__fxstat);
        return UNIFYFS_REAL(__fxstat)(vers, fd, buf);
    }
}
#endif

/* ---------------------------------------
 * POSIX wrappers: file descriptors
 * --------------------------------------- */

/*
 * Read 'count' bytes info 'buf' from file starting at offset 'pos'.
 *
 * Returns number of bytes actually read, or -1 on error, in which
 * case errno will be set.
 */
ssize_t unifyfs_fd_read(int fd, off_t pos, void* buf, size_t count)
{
    /* get the file id for this file descriptor */
    int fid = unifyfs_get_fid_from_fd(fd);
    if (fid < 0) {
        errno = EBADF;
        return -1;
    }

    /* it's an error to read from a directory */
    if (unifyfs_fid_is_dir(fid)) {
        /* TODO: note that read/pread can return this, but not fread */
        errno = EISDIR;
        return -1;
    }

    /* check that file descriptor is open for read */
    unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
    if (!filedesc->read) {
        errno = EBADF;
        return -1;
    }

    /* TODO: is it safe to assume that off_t is bigger than size_t? */
    /* check that we don't overflow the file length */
    if (unifyfs_would_overflow_offt(pos, (off_t) count)) {
        errno = EOVERFLOW;
        return -1;
    }

    /* TODO: check that file is open for reading */

    /* check that we don't try to read past the end of the file */
    off_t lastread = pos + (off_t) count;
    off_t filesize = unifyfs_fid_logical_size(fid);
    if (filesize < lastread) {
        /* adjust count so we don't read past end of file */
        if (filesize > pos) {
            /* read all bytes until end of file */
            count = (size_t)(filesize - pos);
        } else {
            /* pos is already at or past the end of the file */
            count = 0;
        }
    }

    /* if we don't read any bytes, return success */
    if (count == 0) {
        return 0;
    }

    read_req_t tmp_req;
    tmp_req.gfid    = unifyfs_gfid_from_fid(fid);
    tmp_req.offset  = (size_t) pos;
    tmp_req.length  = count;
    tmp_req.errcode = UNIFYFS_SUCCESS;
    tmp_req.buf     = buf;

    int ret = unifyfs_fd_logreadlist(&tmp_req, 1);

    /*
     * FIXME: when we can get the global file size correctly, the following
     * should be rewritten. currently, we cannot detect EOF reliably.
     */
    if (ret != UNIFYFS_SUCCESS) {
        if (tmp_req.errcode != UNIFYFS_SUCCESS) {
            /* error reading data */
            errno = EIO;
            count = -1;
        } else {
            count = 0; /* possible EOF */
        }
    } else {
        /* success, update position */
        filedesc->pos += (off_t) count;
    }
    return count;
}

/*
 * Write 'count' bytes from 'buf' into file starting at offset' pos'.
 * Allocates new bytes and updates file size as necessary.  It is assumed
 * that 'pos' is actually where you want to write, and so O_APPEND behavior
 * is ignored.  Fills any gaps with zeros
 */
int unifyfs_fd_write(int fd, off_t pos, const void* buf, size_t count)
{
    /* get the file id for this file descriptor */
    int fid = unifyfs_get_fid_from_fd(fd);
    if (fid < 0) {
        return UNIFYFS_ERROR_BADF;
    }

    /* it's an error to write to a directory */
    if (unifyfs_fid_is_dir(fid)) {
        return UNIFYFS_ERROR_INVAL;
    }

    /* check that file descriptor is open for write */
    unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
    if (!filedesc->write) {
        return UNIFYFS_ERROR_BADF;
    }

    /* TODO: is it safe to assume that off_t is bigger than size_t? */
    /* check that our write won't overflow the length */
    if (unifyfs_would_overflow_offt(pos, (off_t) count)) {
        /* TODO: want to return EFBIG here for streams */
        return UNIFYFS_ERROR_OVERFLOW;
    }

    /* get current log size before extending the log */
    off_t logsize = unifyfs_fid_log_size(fid);

    /* compute size log will be after we append data */
    off_t newlogsize = logsize + count;

    /* allocate storage space to hold data for this write */
    int extend_rc = unifyfs_fid_extend(fid, newlogsize);
    if (extend_rc != UNIFYFS_SUCCESS) {
        return extend_rc;
    }

    /* finally write specified data to file */
    int write_rc = unifyfs_fid_write(fid, pos, buf, count);
    if (write_rc == 0) {
        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        meta->needs_sync = 1;
        meta->local_size = MAX(meta->local_size, pos + count);
        meta->log_size = newlogsize;
    }
    return write_rc;
}

int UNIFYFS_WRAP(creat)(const char* path, mode_t mode)
{
    /* equivalent to open(path, O_WRONLY|O_CREAT|O_TRUNC, mode) */

    /* check whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* TODO: handle relative paths using current working directory */

        /* create the file */
        int fid;
        off_t pos;
        int rc = unifyfs_fid_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode, &fid, &pos);
        if (rc != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(rc);
            return -1;
        }

        /* allocate a free file descriptor value */
        int fd = unifyfs_stack_pop(unifyfs_fd_stack);
        if (fd < 0) {
            /* ran out of file descriptors */
            errno = EMFILE;
            return -1;
        }

        /* set file id and file pointer, flags include O_WRONLY */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        filedesc->fid   = fid;
        filedesc->pos   = pos;
        filedesc->read  = 0;
        filedesc->write = 1;
        LOGDBG("UNIFYFS_open generated fd %d for file %s", fd, path);

        /* don't conflict with active system fds that range from 0 - (fd_limit) */
        int ret = fd + unifyfs_fd_limit;
        return ret;
    } else {
        MAP_OR_FAIL(creat);
        int ret = UNIFYFS_REAL(creat)(path, mode);
        return ret;
    }
}

int UNIFYFS_WRAP(creat64)(const char* path, mode_t mode)
{
    /* check whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n",
                __FILE__, __LINE__);
        errno = ENOTSUP;
        return -1;
    } else {
        MAP_OR_FAIL(creat64);
        int ret = UNIFYFS_REAL(creat64)(path, mode);
        return ret;
    }
}

int UNIFYFS_WRAP(open)(const char* path, int flags, ...)
{
    /* if O_CREAT is set, we should also have some mode flags */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
    }

    /* determine whether we should intercept this path */
    int ret;
    if (unifyfs_intercept_path(path)) {
        /* TODO: handle relative paths using current working directory */

        /* create the file */
        int fid;
        off_t pos;
        int rc = unifyfs_fid_open(path, flags, mode, &fid, &pos);
        if (rc != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(rc);
            return -1;
        }

        /* allocate a free file descriptor value */
        int fd = unifyfs_stack_pop(unifyfs_fd_stack);
        if (fd < 0) {
            /* ran out of file descriptors */
            errno = EMFILE;
            return -1;
        }

        /* set file id and file pointer */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        filedesc->fid   = fid;
        filedesc->pos   = pos;
        filedesc->read  = ((flags & O_RDONLY) == O_RDONLY)
                          || ((flags & O_RDWR) == O_RDWR);
        filedesc->write = ((flags & O_WRONLY) == O_WRONLY)
                          || ((flags & O_RDWR) == O_RDWR);
        filedesc->append = ((flags & O_APPEND));
        LOGDBG("UNIFYFS_open generated fd %d for file %s", fd, path);

        /* don't conflict with active system fds that range from 0 - (fd_limit) */
        ret = fd + unifyfs_fd_limit;
        return ret;
    } else {
        MAP_OR_FAIL(open);
        if (flags & O_CREAT) {
            ret = UNIFYFS_REAL(open)(path, flags, mode);
        } else {
            ret = UNIFYFS_REAL(open)(path, flags);
        }
        return ret;
    }
}

#ifdef HAVE_OPEN64
int UNIFYFS_WRAP(open64)(const char* path, int flags, ...)
{
    /* if O_CREAT is set, we should also have some mode flags */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
    }

    /* check whether we should intercept this path */
    int ret;
    if (unifyfs_intercept_path(path)) {
        /* Call open wrapper with LARGEFILE flag set*/
        if (flags & O_CREAT) {
            ret = UNIFYFS_WRAP(open)(path, flags | O_LARGEFILE, mode);
        } else {
            ret = UNIFYFS_WRAP(open)(path, flags | O_LARGEFILE);
        }
    } else {
        MAP_OR_FAIL(open64);
        if (flags & O_CREAT) {
            ret = UNIFYFS_REAL(open64)(path, flags, mode);
        } else {
            ret = UNIFYFS_REAL(open64)(path, flags);
        }
    }

    return ret;
}
#endif

int UNIFYFS_WRAP(__open_2)(const char* path, int flags, ...)
{
    int ret;

    LOGDBG("__open_2 was called for path %s", path);

    /* if O_CREAT is set, we should also have some mode flags */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
    }

    /* check whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        LOGDBG("__open_2 was intercepted for path %s", path);

        /* Call open wrapper */
        if (flags & O_CREAT) {
            ret = UNIFYFS_WRAP(open)(path, flags, mode);
        } else {
            ret = UNIFYFS_WRAP(open)(path, flags);
        }
    } else {
        MAP_OR_FAIL(open);
        if (flags & O_CREAT) {
            ret = UNIFYFS_REAL(open)(path, flags, mode);
        } else {
            ret = UNIFYFS_REAL(open)(path, flags);
        }
    }

    return ret;
}

off_t UNIFYFS_WRAP(lseek)(int fd, off_t offset, int whence)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* TODO: check that fd is actually in use */

        /* get the file id for this file descriptor */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            /* bad file descriptor */
            errno = EBADF;
            return (off_t)(-1);
        }

        /* lookup meta to get file size */
        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* bad file descriptor */
            errno = EBADF;
            return (off_t)(-1);
        }

        /* get file descriptor for fd */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);

        /* get current file position */
        off_t current_pos = filedesc->pos;

        /* TODO: support SEEK_DATA and SEEK_HOLE? */

        /* compute final file position */
        switch (whence) {
        case SEEK_SET:
            /* seek to offset */
            current_pos = offset;
            break;
        case SEEK_CUR:
            /* seek to current position + offset */
            current_pos += offset;
            break;
        case SEEK_END:
            /* seek to EOF + offset */
            current_pos = unifyfs_fid_logical_size(fid);
            break;
        default:
            errno = EINVAL;
            return (off_t)(-1);
        }

        /* set and return final file position */
        filedesc->pos = current_pos;
        return current_pos;
    } else {
        MAP_OR_FAIL(lseek);
        off_t ret = UNIFYFS_REAL(lseek)(fd, offset, whence);
        return ret;
    }
}

off64_t UNIFYFS_WRAP(lseek64)(int fd, off64_t offset, int whence)
{
    /* check whether we should intercept this file descriptor */
    int origfd = fd;
    if (unifyfs_intercept_fd(&fd)) {
        if (sizeof(off_t) == sizeof(off64_t)) {
            /* off_t and off64_t are the same size,
             * delegate to lseek warpper */
            off64_t ret = (off64_t)UNIFYFS_WRAP(lseek)(
                              origfd, (off_t) offset, whence);
            return ret;
        } else {
            /* ERROR: fn not yet supported */
            fprintf(stderr, "Function not yet supported @ %s:%d\n",
                    __FILE__, __LINE__);
            errno = ENOTSUP;
            return (off64_t)(-1);
        }
    } else {
        MAP_OR_FAIL(lseek64);
        off64_t ret = UNIFYFS_REAL(lseek64)(fd, offset, whence);
        return ret;
    }
}

#ifdef HAVE_POSIX_FADVISE
int UNIFYFS_WRAP(posix_fadvise)(int fd, off_t offset, off_t len, int advice)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* check that the file descriptor is valid */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            errno = EBADF;
            return errno;
        }

        /* process advice from caller */
        switch (advice) {
        case POSIX_FADV_NORMAL:
        case POSIX_FADV_SEQUENTIAL:
        /* can use this hint for a better compression strategy */
        case POSIX_FADV_RANDOM:
        case POSIX_FADV_NOREUSE:
        case POSIX_FADV_WILLNEED:
        /* with the spill-over case, we can use this hint to
         * to better manage the in-memory parts of a file. On
         * getting this advice, move the chunks that are on the
         * spill-over device to the in-memory portion
         */
        case POSIX_FADV_DONTNEED:
            /* similar to the previous case, but move contents from memory
             * to the spill-over device instead.
             */

            /* ERROR: fn not yet supported */
            fprintf(stderr, "Function not yet supported @ %s:%d\n",
                    __FILE__, __LINE__);
            errno = ENOTSUP;
            return errno;
        default:
            /* this function returns the errno itself, not -1 */
            errno = EINVAL;
            return errno;
        }

        /* just a hint so return success even if we don't do anything */
        return 0;
    } else {
        MAP_OR_FAIL(posix_fadvise);
        int ret = UNIFYFS_REAL(posix_fadvise)(fd, offset, len, advice);
        return ret;
    }
}
#endif

ssize_t UNIFYFS_WRAP(read)(int fd, void* buf, size_t count)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* get file id */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

        /* get pointer to file descriptor structure */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        if (filedesc == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

#if 0 // THIS IS BROKEN UNTIL WE HAVE GLOBAL SIZE
        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

        /* check for end of file */
        if (filedesc->pos >= meta->size) {
            return 0;   /* EOF */
        }
#endif
        return unifyfs_fd_read(fd, filedesc->pos, buf, count);
    } else {
        MAP_OR_FAIL(read);
        ssize_t ret = UNIFYFS_REAL(read)(fd, buf, count);
        return ret;
    }
}

/* TODO: find right place to msync spillover mapping */
ssize_t UNIFYFS_WRAP(write)(int fd, const void* buf, size_t count)
{
    size_t ret;
    off_t pos;

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {

        LOGDBG("write %d bytes to fd %d", count, fd);
        /* get pointer to file descriptor structure */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        if (filedesc == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

        if (filedesc->append) {
            /*
             * With O_APPEND we always write to the end, despite the current
             * file position.
             */
            int fid = unifyfs_get_fid_from_fd(fd);
            pos = unifyfs_fid_local_size(fid);
        } else {
            pos = filedesc->pos;
        }

        /* write data to file */
        int write_rc = unifyfs_fd_write(fd, pos, buf, count);
        if (write_rc != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(write_rc);
            return (ssize_t)(-1);
        }
        ret = count;

        /* update file position */
        filedesc->pos = pos + count;
    } else {
        MAP_OR_FAIL(write);
        ret = UNIFYFS_REAL(write)(fd, buf, count);
    }

    return ret;
}

ssize_t UNIFYFS_WRAP(readv)(int fd, const struct iovec* iov, int iovcnt)
{
    ssize_t ret;

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        ssize_t rret;
        int i;
        ret = 0;
        for (i = 0; i < iovcnt; i++) {
            rret = UNIFYFS_WRAP(read)(fd, (void*)iov[i].iov_base,
                                      iov[i].iov_len);
            if (-1 == rret) {
                return -1;
            } else if (0 == rret) {
                return ret;
            } else {
                ret += rret;
            }
        }
        return ret;
    } else {
        MAP_OR_FAIL(readv);
        ret = UNIFYFS_REAL(readv)(fd, iov, iovcnt);
        return ret;
    }
}

ssize_t UNIFYFS_WRAP(writev)(int fd, const struct iovec* iov, int iovcnt)
{
    ssize_t ret;

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        ssize_t wret;
        int i;
        ret = 0;
        for (i = 0; i < iovcnt; i++) {
            wret = UNIFYFS_WRAP(write)(fd, (const void*)iov[i].iov_base,
                                       iov[i].iov_len);
            if (-1 == wret) {
                return -1;
            } else {
                ret += wret;
                if ((size_t)wret != iov[i].iov_len) {
                    return ret;
                }
            }
        }
        return ret;
    } else {
        MAP_OR_FAIL(writev);
        ret = UNIFYFS_REAL(writev)(fd, iov, iovcnt);
        return ret;
    }
}

#ifdef HAVE_LIO_LISTIO
int UNIFYFS_WRAP(lio_listio)(int mode, struct aiocb* const aiocb_list[],
                             int nitems, struct sigevent* sevp)
{
    /* TODO - support for LIO_NOWAIT mode */

    read_req_t* reqs = calloc(nitems, sizeof(read_req_t));
    if (NULL == reqs) {
        errno = ENOMEM; // EAGAIN?
        return -1;
    }

    int ret = 0;
    int reqcnt = 0;
    int i, fd, fid, ndx, rc;
    struct aiocb* cbp;

    for (i = 0; i < nitems; i++) {
        cbp = aiocb_list[i];
        fd = cbp->aio_fildes;
        switch (cbp->aio_lio_opcode) {
        case LIO_WRITE: {
            ssize_t wret;
            wret = UNIFYFS_WRAP(pwrite)(fd, (const void*)cbp->aio_buf,
                                        cbp->aio_nbytes, cbp->aio_offset);
            if (-1 == wret) {
                AIOCB_ERROR_CODE(cbp) = errno;
            } else {
                AIOCB_ERROR_CODE(cbp) = 0;
                AIOCB_RETURN_VAL(cbp) = wret;
            }
            break;
        }
        case LIO_READ: {
            if (unifyfs_intercept_fd(&fd)) {
                /* get local file id for this request */
                fid = unifyfs_get_fid_from_fd(fd);
                if (fid < 0) {
                    AIOCB_ERROR_CODE(cbp) = EINVAL;
                } else {
                    reqs[reqcnt].gfid    = unifyfs_gfid_from_fid(fid);
                    reqs[reqcnt].offset  = (size_t)(cbp->aio_offset);
                    reqs[reqcnt].length  = cbp->aio_nbytes;
                    reqs[reqcnt].errcode = EINPROGRESS;
                    reqs[reqcnt].buf     = (char*)(cbp->aio_buf);
                    reqcnt++;
                }
            } else {
                ssize_t rret;
                rret = UNIFYFS_WRAP(pread)(fd, (void*)cbp->aio_buf,
                                           cbp->aio_nbytes, cbp->aio_offset);
                if (-1 == rret) {
                    AIOCB_ERROR_CODE(cbp) = errno;
                } else {
                    AIOCB_ERROR_CODE(cbp) = 0;
                    AIOCB_RETURN_VAL(cbp) = rret;
                }
            }
            break;
        }
        default: // LIO_NOP
            break;
        }
    }

    if (reqcnt) {
        rc = unifyfs_fd_logreadlist(reqs, reqcnt);
        if (rc != UNIFYFS_SUCCESS) {
            /* error reading data */
            ret = -1;
        }
        /* update aiocb fields to record error status and return value */
        ndx = 0;
        for (i = 0; i < reqcnt; i++) {
            char* buf = reqs[i].buf;
            for (; ndx < nitems; ndx++) {
                cbp = aiocb_list[ndx];
                if ((char*)(cbp->aio_buf) == buf) {
                    AIOCB_ERROR_CODE(cbp) = reqs[i].errcode;
                    if (0 == reqs[i].errcode) {
                        AIOCB_RETURN_VAL(cbp) = reqs[i].length;
                    }
                    break; // continue outer loop
                }
            }
        }
    }

    free(reqs);

    if (-1 == ret) {
        errno = EIO;
    }
    return ret;
}
#endif

/* order by file id then by file position */
static int compare_index_entry(const void* a, const void* b)
{
    const unifyfs_index_t* ptr_a = a;
    const unifyfs_index_t* ptr_b = b;

    if (ptr_a->gfid != ptr_b->gfid) {
        if (ptr_a->gfid < ptr_b->gfid) {
            return -1;
        } else {
            return 1;
        }
    }

    if (ptr_a->file_pos == ptr_b->file_pos) {
        return 0;
    } else if (ptr_a->file_pos < ptr_b->file_pos) {
        return -1;
    } else {
        return 1;
    }
}

/* order by file id then by offset */
static int compare_read_req(const void* a, const void* b)
{
    const read_req_t* ptr_a = a;
    const read_req_t* ptr_b = b;

    if (ptr_a->gfid != ptr_b->gfid) {
        if (ptr_a->gfid < ptr_b->gfid) {
            return -1;
        } else {
            return 1;
        }
    }

    if (ptr_a->offset == ptr_b->offset) {
        return 0;
    } else if (ptr_a->offset < ptr_b->offset) {
        return -1;
    } else {
        return 1;
    }
}

/* returns index into read_req of item whose offset is
 * just below offset of target item (if one exists) */
static int unifyfs_locate_req(read_req_t* read_reqs, int count,
                              read_req_t* match_req)
{
    /* if list is empty, indicate that there is valid starting request */
    if (count == 0) {
        return -1;
    }

    /* if we only have one item, return its index */
    if (count == 1) {
        return 0;
    }

    /* if we have two items, return index to item that must come before */
    if (count == 2) {
        if (compare_read_req(match_req, &read_reqs[1]) < 0) {
            /* second item is clearly bigger, so try first */
            return 0;
        }

        /* second item is less than or equal to target */
        return 1;
    }

    /* execute binary search comparing target to list of requests */

    int left  = 0;
    int right = count - 1;
    int mid   = (left + right) / 2;

    /* binary search until we find an exact match or have cut the list
     * to just two items */
    int cmp;
    while ((left + 1) < right) {
        cmp = compare_read_req(match_req, &read_reqs[mid]);
        if (cmp == 0) {
            /* found exact match */
            return mid;
        } else if (cmp > 0) {
            /* if target if bigger than mid, set left bound to mid */
            left = mid;
        } else {
            /* if target is smaller than mid, set right bounds to mid */
            right = mid;
        }

        /* update middle index */
        mid = (left + right) / 2;
    }

    /* got two items, let's pick one */
    if (compare_read_req(match_req, &read_reqs[left]) < 0) {
        /* target is smaller than left item,
         * return index to left of left item if we can */
        if (left == 0) {
            /* at left most item, so return this index */
            return 0;
        }
        return left - 1;
    } else if (compare_read_req(match_req, &read_reqs[right]) < 0) {
        /* target is smaller than right item,
         * return index of item one less than right */
        return right - 1;
    } else {
        /* target is greater or equal to right item */
        return right;
    }
}

/* given a read request, split it into multiple requests whose range
 * is equal or smaller than slice_range size
 *
 * @param  req:         read request to be split
 * @param  slice_range: slice size of the key-value store
 * @return out_set:     output set of split read requests
 * @param  maxcount:    number of entries in output array
 * @return used_count:  number of entries added to output array */
static int unifyfs_split_read_request(
    read_req_t* in_req,  /* read request to split */
    long slice_range,    /* number of bytes in each slice */
    read_req_t* out_set, /* output array to store new requests in */
    off_t maxcount,      /* max number of items in output array */
    off_t* used_count)   /* number of entries we added in split */
{
    /* check that we have at least one spot in buffer */
    if (maxcount == 0) {
        *used_count = 0;
        return UNIFYFS_FAILURE;
    }

    /* make a copy of the input request so we can modify it */
    read_req_t tmp_req = *in_req;
    read_req_t* req = &tmp_req;

    /* first byte offset this request will read from */
    size_t req_start = req->offset;

    /* last byte offset this request will read from */
    size_t req_end = req->offset + req->length - 1;

    /* compute offset of first and last byte of slice
     * that contains first byte of request */

    /* starting byte offset of slice that first read offset falls in */
    size_t slice_start = (req->offset / slice_range) * slice_range;

    /* last byte offset of slice that first read offset falls in */
    size_t slice_end = slice_start + slice_range - 1;

    /* initialize request count in output set */
    int count = 0;

    /* define new read requests in out_set by splitting request
     * at slice boundaries */
    if (req_end <= slice_end) {
        /* slice fully contains request
         *
         * slice_start           slice_end
         *      req_start   req_end
         */
        out_set[count] = *req;
        count++;
    } else {
        /* ending offset of request is beyond last offset in first slice,
         * so this request spans across multiple slices
         *
         * slice_start  slice_end  next_slice_start      next_slice_end
         *      req_start                          req_end
         *
         */

        /* compute number of bytes until end of first slice */
        long length = slice_end - req_start + 1;

        out_set[count].gfid    = req->gfid;
        out_set[count].offset  = req->offset;
        out_set[count].length  = length;
        out_set[count].errcode = req->errcode;
        count++;

        /* update write index to account for index we just added */
        req->offset += length;
        req->length -= length;

        /* check that we have room to write more requests */
        if (count >= maxcount) {
            /* no room to write more requests,
             * and we have at least one more,
             * record number we wrote and return with error */
            *used_count = count;
            return UNIFYFS_FAILURE;
        }

        /* advance slice boundary offsets to next slice */
        slice_end += slice_range;

        /* loop until we find the slice that contains
         * ending offset of read */
        while (req_end > slice_end) {
            /* ending offset of read is beyond end of this slice,
             * so read spans the full length of this slice */
            length = slice_range;

            /* full slice is contained in read request */
            out_set[count].gfid    = req->gfid;
            out_set[count].offset  = req->offset;
            out_set[count].length  = length;
            out_set[count].errcode = req->errcode;
            count++;

            /* update read request to account for index we just added */
            req->offset += length;
            req->length -= length;

            /* check that we have room to write more requests */
            if (count >= maxcount) {
                /* no room to write more requests,
                 * and we have at least one more,
                 * record number we wrote and return with error */
                *used_count = count;
                return UNIFYFS_FAILURE;
            }

            /* advance slice boundary offsets to next slice */
            slice_end += slice_range;
        }

        /* this slice contains the remainder of read */
        length = req->length;
        out_set[count].gfid    = req->gfid;
        out_set[count].offset  = req->offset;
        out_set[count].length  = length;
        out_set[count].errcode = req->errcode;
        count++;

        /* update read request to account for index we just added */
        req->offset += length;
        req->length -= length;
    }

    /* record number of entries we added */
    *used_count = count;

    return UNIFYFS_SUCCESS;
}

/*
 * match the received read_requests with the
 * client's read requests
 * @param read_reqs: a list of read requests
 * @param count: number of read requests
 * @param match_req: received read request to match
 * @return error code
 *
 * */
static int unifyfs_match_received_ack(read_req_t* read_reqs, int count,
                                      read_req_t* match_req)
{
    /* given fid, offset, and length of match_req that holds read reply,
     * identify which read request this belongs to in read_req array,
     * then copy data to user buffer */

    /* create a request corresponding to the first byte in read reply */
    read_req_t match_start = *match_req;

    /* create a request corresponding to last byte in read reply */
    read_req_t match_end = *match_req;
    match_end.offset += match_end.length - 1;

    /* find index of read request that contains our first byte */
    int start_pos = unifyfs_locate_req(read_reqs, count, &match_start);

    /* find index of read request that contains our last byte */
    int end_pos = unifyfs_locate_req(read_reqs, count, &match_end);

    /* could not find a valid read request in read_req array */
    if (start_pos == -1) {
        return UNIFYFS_FAILURE;
    }

    /* s: start of match_req, e: end of match_req */

    if (start_pos == 0) {
        if (compare_read_req(&match_start, &read_reqs[0]) < 0) {
            /* starting offset in read reply comes before lowest
             * offset in read requests, consider this to be an error
             *
             *   ************    ***********         *************
             * s
             *
             * */
            return UNIFYFS_FAILURE;
        }
    }

    /* create read request corresponding to first byte of first read request */
    read_req_t first_start = read_reqs[start_pos];

    /* create read request corresponding to last byte of first read request */
    read_req_t first_end = read_reqs[start_pos];
    first_end.offset += first_end.length - 1;

    /* check whether read reply is fully contained by first read request */
    if (compare_read_req(&match_start, &first_start) >= 0 &&
        compare_read_req(&match_end,   &first_end)   <= 0) {
        /* read reply is fully contained within first read request
         *
         * first_s   first_e
         * *****************           *************
         *        s  e
         *
         * */

        /* copy data to user buffer if no error */
        if (match_req->errcode == UNIFYFS_SUCCESS) {
            /* compute buffer location to copy data */
            size_t offset = (size_t)(match_start.offset - first_start.offset);
            char* buf = first_start.buf + offset;

            /* copy data to user buffer */
            memcpy(buf, match_req->buf, match_req->length);

            return UNIFYFS_SUCCESS;
        } else {
            /* hit an error during read, so record this fact
             * in user's original read request */
            read_reqs[start_pos].errcode = match_req->errcode;
            return UNIFYFS_FAILURE;
        }
    }

    /* define read request for offset of first byte in last read request */
    read_req_t last_start = read_reqs[end_pos];

    /* define read request for offset of last byte in last read request */
    read_req_t last_end = read_reqs[end_pos];
    last_end.offset += last_end.length - 1;

    /* determine whether read reply is contained in a range of read requests */
    if (compare_read_req(&match_start, &first_start) >= 0 &&
        compare_read_req(&match_end,   &last_end)    <= 0) {
        /* read reply spans multiple read requests
         *
         *  first_s   first_e  req_s req_e  req_s req_e  last_s    last_e
         *  *****************  ***********  ***********  ****************
         *          s                                              e
         *
         * */

        /* check that read requests from start_pos to end_pos
         * define a contiguous set of bytes */
        int i;
        for (i = start_pos + 1; i <= end_pos; i++) {
            if ((read_reqs[i - 1].offset + read_reqs[i - 1].length)
                != read_reqs[i].offset) {
                /* read requests are noncontiguous, error */
                return UNIFYFS_FAILURE;
            }
        }

        /* read requests are contiguous, fill all buffers in middle */
        if (match_req->errcode == UNIFYFS_SUCCESS) {
            /* get pointer to start of read reply data */
            char* ptr = match_req->buf;

            /* compute position in user buffer to copy data */
            size_t offset = (size_t)(match_start.offset - first_start.offset);
            char* buf = first_start.buf + offset;

            /* compute number of bytes to copy into first read request */
            size_t length =
                (size_t)(first_end.offset - match_start.offset + 1);

            /* copy data into user buffer for first read request */
            memcpy(buf, ptr, length);
            ptr += length;

            /* copy data for middle read requests */
            for (i = start_pos + 1; i < end_pos; i++) {
                memcpy(read_reqs[i].buf, ptr, read_reqs[i].length);
                ptr += read_reqs[i].length;
            }

            /* compute bytes for last read request */
            length = (size_t)(match_end.offset - last_start.offset + 1);

            /* copy data into user buffer for last read request */
            memcpy(last_start.buf, ptr, length);
            ptr += length;

            return UNIFYFS_SUCCESS;
        } else {
            /* hit an error during read, update errcode in user's
             * original read request from start to end inclusive */
            for (i = start_pos; i <= end_pos; i++) {
                read_reqs[i].errcode = match_req->errcode;
            }
            return UNIFYFS_FAILURE;
        }
    }

    /* could not find a matching read request, return an error */
    return UNIFYFS_FAILURE;
}

/* notify our delegator that the shared memory buffer
 * is now clear and ready to hold more read data */
static void delegator_signal(void)
{
    LOGDBG("receive buffer now empty");

    /* set shm flag to signal delegator we're done */
    shm_header_t* hdr = (shm_header_t*)shm_recv_buf;
    hdr->state = SHMEM_REGION_EMPTY;

    /* TODO: MEM_FLUSH */
}

/* wait for delegator to inform us that shared memory buffer
 * is filled with read data */
static int delegator_wait(void)
{
    int rc = (int)UNIFYFS_SUCCESS;

#if defined(UNIFYFS_USE_DOMAIN_SOCKET)
    /* wait for signal on socket */
    cmd_fd.events = POLLIN | POLLPRI;
    cmd_fd.revents = 0;
    rc = poll(&cmd_fd, 1, -1);

    /* check that we got something good */
    if (rc == 0) {
        if (cmd_fd.revents != 0) {
            if (cmd_fd.revents == POLLIN) {
                return UNIFYFS_SUCCESS;
            } else {
                printf("poll returned %d; error: %s\n", rc,  strerror(errno));
            }
        } else {
            printf("poll returned %d; error: %s\n", rc,  strerror(errno));
        }
    } else {
        printf("poll returned %d; error: %s\n", rc,  strerror(errno));
    }
#endif

    /* specify time to sleep between checking flag in shared
     * memory indicating server has produced */
    struct timespec shm_wait_tm;
    shm_wait_tm.tv_sec  = 0;
    shm_wait_tm.tv_nsec = SHM_WAIT_INTERVAL;

    /* get pointer to flag in shared memory */
    shm_header_t* hdr = (shm_header_t*)shm_recv_buf;

    /* wait for server to set flag to non-zero */
    int max_sleep = 5000000; // 5s
    volatile int* vip = (volatile int*)&(hdr->state);
    while (*vip == SHMEM_REGION_EMPTY) {
        /* not there yet, sleep for a while */
        nanosleep(&shm_wait_tm, NULL);
        /* TODO: MEM_FETCH */
        max_sleep--;
        if (0 == max_sleep) {
            LOGERR("timed out waiting for non-empty");
            rc = (int)UNIFYFS_ERROR_SHMEM;
            break;
        }
    }

    return rc;
}

/* copy read data from shared memory buffer to user buffers from read
 * calls, sets done=1 on return when delegator informs us it has no
 * more data */
static int process_read_data(read_req_t* read_reqs, int count, int* done)
{
    /* assume we'll succeed */
    int rc = UNIFYFS_SUCCESS;

    /* get pointer to start of shared memory buffer */
    shm_header_t* shm_hdr = (shm_header_t*)shm_recv_buf;
    char* shmptr = ((char*)shm_hdr) + sizeof(shm_header_t);

    size_t num = shm_hdr->meta_cnt;
    if (0 == num) {
        LOGDBG("no read responses available");
        return rc;
    }

    /* process each of our replies */
    size_t i;
    for (i = 0; i < num; i++) {
        /* get pointer to current read reply header */
        shm_meta_t* msg = (shm_meta_t*)shmptr;
        shmptr += sizeof(shm_meta_t);

        /* define request object */
        read_req_t req;
        req.gfid    = msg->gfid;
        req.offset  = msg->offset;
        req.length  = msg->length;
        req.errcode = msg->errcode;

        /* get pointer to data */
        req.buf = shmptr;
        shmptr += msg->length;

        /* process this read reply, identify which application read
         * request this reply goes to and copy data to user buffer */
        int tmp_rc = unifyfs_match_received_ack(read_reqs, count, &req);
        if (tmp_rc != UNIFYFS_SUCCESS) {
            rc = UNIFYFS_FAILURE;
        }
    }

    /* set done flag if there is no more data */
    if (shm_hdr->state == SHMEM_REGION_DATA_COMPLETE) {
        *done = 1;
    }

    return rc;
}

/*
 * get data for a list of read requests from the
 * delegator
 * @param read_reqs: a list of read requests
 * @param count: number of read requests
 * @return error code
 *
 * */
int unifyfs_fd_logreadlist(read_req_t* read_reqs, int count)
{
    int read_rc;

    int rc = UNIFYFS_SUCCESS;

    /*
     * Todo: When the number of read requests exceed the
     * request buffer, split list io into multiple bulk
     * sends and transfer in bulks
     * */

    /* order read request by increasing file id, then increasing offset */
    qsort(read_reqs, count, sizeof(read_req_t), compare_read_req);

    /* TODO: move this split code to server and then pass original
     * read requests from client to server */
    /* split read requests at file offset boundaries used internally
     * in the server key/value store */
    int i;
    off_t req_count = 0;
    read_req_t read_set[UNIFYFS_MAX_READ_CNT];
    for (i = 0; i < count; i++) {
        /* remaining entries we have in our read requests array */
        off_t remaining = UNIFYFS_MAX_READ_CNT - req_count;

        /* split current request at key/value offsets */
        off_t used = 0;
        read_rc = unifyfs_split_read_request(&read_reqs[i],
            unifyfs_key_slice_range, &read_set[req_count], remaining, &used);

        /* bail out with error if we failed to process read requests */
        if (read_rc != UNIFYFS_SUCCESS) {
            LOGERR("Failed to split read requests");
            return read_rc;
        }

        /* account of request slots we used up */
        req_count += used;
    }

    /* prepare our shared memory buffer for delegator */
    delegator_signal();

    if (req_count > 1) {
        /* got multiple read requests,
         * build up a flat buffer to include them all */
        flatcc_builder_t builder;
        flatcc_builder_init(&builder);

        /* create request vector */
        unifyfs_Extent_vec_start(&builder);

        /* fill in values for each request entry */
        for (i = 0; i < req_count; i++) {
            unifyfs_Extent_vec_push_create(&builder,
                                           read_set[i].gfid,
                                           read_set[i].offset,
                                           read_set[i].length);
        }

        /* complete the array */
        unifyfs_Extent_vec_ref_t extents = unifyfs_Extent_vec_end(&builder);
        unifyfs_ReadRequest_create_as_root(&builder, extents);
        //unifyfs_ReadRequest_end_as_root(&builder);

        /* allocate our buffer to be sent */
        size_t size = 0;
        void* buffer = flatcc_builder_finalize_buffer(&builder, &size);
        assert(buffer);
        LOGDBG("mread: n_reqs:%d, flatcc buffer (%p) sz:%zu",
               req_count, buffer, size);

        /* invoke read rpc here */
        read_rc = invoke_client_mread_rpc(req_count, size, buffer);

        flatcc_builder_clear(&builder);
        free(buffer);
    } else {
        /* got a single read request */
        int gfid      = read_set[0].gfid;
        size_t offset = read_set[0].offset;
        size_t length = read_set[0].length;
        LOGDBG("read: offset:%zu, len:%zu", offset, length);
        read_rc = invoke_client_read_rpc(gfid, offset, length);
    }

    /* bail out with error if we failed to even start the read */
    if (read_rc != UNIFYFS_SUCCESS) {
        LOGERR("Failed to issue read RPC to server");
        return read_rc;
    }

    /*
     * ToDo: Exception handling when some of the requests
     * are missed
     * */
    int done = 0;
    while (!done) {
        int tmp_rc = delegator_wait();
        if (tmp_rc != UNIFYFS_SUCCESS) {
            rc = UNIFYFS_FAILURE;
            done = 1;
        } else {
            tmp_rc = process_read_data(read_reqs, count, &done);
            if (tmp_rc != UNIFYFS_SUCCESS) {
                rc = UNIFYFS_FAILURE;
            }
            delegator_signal();
        }
    }

    return rc;
}

ssize_t UNIFYFS_WRAP(pread)(int fd, void* buf, size_t count, off_t offset)
{
    /* equivalent to read(), except that it shall read from a given
     * position in the file without changing the file pointer */

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* get file id */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

#if 0 // THIS IS BROKEN UNTIL WE HAVE GLOBAL SIZE
        /* get pointer to file descriptor structure */
        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

        /* check for end of file */
        if (offset >= meta->size) {
            return 0;
        }
#endif

        /* assume we'll succeed in read */
        size_t retcount = count;

        read_req_t tmp_req;
        tmp_req.gfid    = unifyfs_gfid_from_fid(fid);
        tmp_req.offset  = offset;
        tmp_req.length  = count;
        tmp_req.errcode = UNIFYFS_SUCCESS;
        tmp_req.buf     = buf;

        int ret = unifyfs_fd_logreadlist(&tmp_req, 1);
        if (ret != UNIFYFS_SUCCESS) {
            /* error reading data */
            errno = EIO;
            retcount = -1;
        } else if (tmp_req.errcode != UNIFYFS_SUCCESS) {
            /* error reading data */
            errno = EIO;
            retcount = -1;
        }

        /* return number of bytes read */
        return (ssize_t) retcount;
    } else {
        MAP_OR_FAIL(pread);
        ssize_t ret = UNIFYFS_REAL(pread)(fd, buf, count, offset);
        return ret;
    }
}

ssize_t UNIFYFS_WRAP(pread64)(int fd, void* buf, size_t count, off64_t offset)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        return UNIFYFS_WRAP(pread)(fd, buf, count, (off_t)offset);
    } else {
        MAP_OR_FAIL(pread64);
        ssize_t ret = UNIFYFS_REAL(pread64)(fd, buf, count, offset);
        return ret;
    }
}

ssize_t UNIFYFS_WRAP(pwrite)(int fd, const void* buf, size_t count,
                             off_t offset)
{
    /* equivalent to write(), except that it writes into a given
     * position without changing the file pointer */
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* get pointer to file descriptor structure */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        if (filedesc == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return (ssize_t)(-1);
        }

        /* write data to file */
        int write_rc = unifyfs_fd_write(fd, offset, buf, count);
        if (write_rc != UNIFYFS_SUCCESS) {
            errno = unifyfs_err_map_to_errno(write_rc);
            return (ssize_t)(-1);
        }

        /* return number of bytes read */
        return (ssize_t) count;
    } else {
        MAP_OR_FAIL(pwrite);
        ssize_t ret = UNIFYFS_REAL(pwrite)(fd, buf, count, offset);
        return ret;
    }
}

ssize_t UNIFYFS_WRAP(pwrite64)(int fd, const void* buf, size_t count,
                               off64_t offset)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        return UNIFYFS_WRAP(pwrite)(fd, buf, count, (off_t)offset);
    } else {
        MAP_OR_FAIL(pwrite64);
        ssize_t ret = UNIFYFS_REAL(pwrite64)(fd, buf, count, offset);
        return ret;
    }
}

int UNIFYFS_WRAP(ftruncate)(int fd, off_t length)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* get the file id for this file descriptor */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return -1;
        }

        /* check that file descriptor is open for write */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        if (!filedesc->write) {
            errno = EBADF;
            return -1;
        }

        /* truncate the file */
        int rc = unifyfs_fid_truncate(fid, length);
        if (rc != UNIFYFS_SUCCESS) {
            errno = EIO;
            return -1;
        }

        return 0;
    } else {
        MAP_OR_FAIL(ftruncate);
        int ret = UNIFYFS_REAL(ftruncate)(fd, length);
        return ret;
    }
}

int UNIFYFS_WRAP(fsync)(int fd)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {

        if (*unifyfs_indices.ptr_num_entries == 0) {
            /* Nothing to sync */
            return 0;
        }

        /* get the file id for this file descriptor */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            LOGERR("Couldn't get fid from fd %d", fd);
            errno = EBADF;
            return -1;
        }

        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        if (!meta->needs_sync) {
            return 0;
        }

        /* if using spill over, fsync spillover data to disk */
        if (unifyfs_use_spillover) {
            int ret = __real_fsync(unifyfs_spilloverblock);
            if (ret != 0) {
                /* error, need to set errno appropriately,
                 * we called the real fsync which should
                 * have already set errno to something reasonable */
                return -1;
            }
        }

        if (unifyfs_flatten_writes) {
            unifyfs_rewrite_index_from_seg_tree();
        }

        /* invoke fsync rpc to register index metadata with server */
        int gfid = unifyfs_gfid_from_fid(fid);
        int ret = invoke_client_fsync_rpc(gfid);
        if (ret != UNIFYFS_SUCCESS) {
            /* sync failed for some reason, set errno and return error */
            errno = unifyfs_err_map_to_errno(ret);
            return -1;
        }

        /* server has processed entries in index buffer, reset it */
        *(unifyfs_indices.ptr_num_entries) = 0;

        meta->needs_sync = 0;
        return 0;
    } else {
        MAP_OR_FAIL(fsync);
        int ret = UNIFYFS_REAL(fsync)(fd);
        return ret;
    }
}

int UNIFYFS_WRAP(fdatasync)(int fd)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n",
                __FILE__, __LINE__);
        errno = ENOTSUP;
        return -1;
    } else {
        MAP_OR_FAIL(fdatasync);
        int ret = UNIFYFS_REAL(fdatasync)(fd);
        return ret;
    }
}

int UNIFYFS_WRAP(flock)(int fd, int operation)
{
    int ret;

    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        // KMM I removed the locking code because it was causing
        // hangs
        /*
          -- currently handling the blocking variants only
          switch (operation)
          {
              case LOCK_EX:
                  LOGDBG("locking file %d", fid);
                  ret = pthread_spin_lock(&meta->fspinlock);
                  if ( ret ) {
                      perror("pthread_spin_lock() failed");
                      return -1;
                  }
                  meta->flock_status = EX_LOCKED;
                  break;
              case LOCK_SH:
                  -- not needed for CR; will not be supported,
                  --  update flock_status anyway
                  meta->flock_status = SH_LOCKED;
                  break;
              case LOCK_UN:
                  ret = pthread_spin_unlock(&meta->fspinlock);
                  LOGDBG("unlocking file %d", fid);
                  meta->flock_status = UNLOCKED;
                  break;
              default:
                  errno = EINVAL;
                  return -1;
          }
         */

        return 0;
    } else {
        MAP_OR_FAIL(flock);
        ret = UNIFYFS_REAL(flock)(fd, operation);
        return ret;
    }
}

/* TODO: handle different flags */
void* UNIFYFS_WRAP(mmap)(void* addr, size_t length, int prot, int flags,
                         int fd, off_t offset)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* for now, tell user that we can't support mmap,
         * we'll need to track assigned memory region so that
         * we can identify our files on msync and munmap */
        fprintf(stderr, "Function not yet supported @ %s:%d\n",
                __FILE__, __LINE__);
        errno = ENODEV;
        return MAP_FAILED;

#if 0 // TODO - mmap support
        /* get the file id for this file descriptor */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            errno = EBADF;
            return MAP_FAILED;
        }

        /* TODO: handle addr properly based on flags */

        /* allocate memory required to mmap the data if addr is NULL;
         * using posix_memalign instead of malloc to align mmap'ed area
         * to page size */
        if (!addr) {
            int ret = posix_memalign(&addr, sysconf(_SC_PAGE_SIZE), length);
            if (ret) {
                /* posix_memalign does not set errno */
                if (ret == EINVAL) {
                    errno = EINVAL;
                    return MAP_FAILED;
                }

                if (ret == ENOMEM) {
                    errno = ENOMEM;
                    return MAP_FAILED;
                }
            }
        }

        /* TODO: do we need to extend file if offset+length goes past current end? */

        /* check that we don't copy past the end of the file */
        off_t last_byte = offset + length;
        off_t file_size = unifyfs_fid_size(fid);
        if (last_byte > file_size) {
            /* trying to copy past the end of the file, so
             * adjust the total amount to be copied */
            length = (size_t)(file_size - offset);
        }

        /* read data from file */
        int rc = unifyfs_fid_read(fid, offset, addr, length);
        if (rc != UNIFYFS_SUCCESS) {
            /* TODO: need to free memory in this case? */
            errno = ENOMEM;
            return MAP_FAILED;
        }

        return addr;
#endif
    } else {
        MAP_OR_FAIL(mmap);
        void* ret = UNIFYFS_REAL(mmap)(addr, length, prot, flags, fd, offset);
        return ret;
    }
}

int UNIFYFS_WRAP(munmap)(void* addr, size_t length)
{
#if 0 // TODO - mmap support
    fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
    errno = EINVAL;
    return -1;
#endif
    MAP_OR_FAIL(munmap);
    int ret = UNIFYFS_REAL(munmap)(addr, length);
    return ret;
}

int UNIFYFS_WRAP(msync)(void* addr, size_t length, int flags)
{
#if 0 // TODO - mmap support
    /* TODO: need to keep track of all the mmaps that are linked to
     * a given file before this function can be implemented */
    fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
    errno = EINVAL;
    return -1;
#endif
    MAP_OR_FAIL(msync);
    int ret = UNIFYFS_REAL(msync)(addr, length, flags);
    return ret;
}

void* UNIFYFS_WRAP(mmap64)(void* addr, size_t length, int prot, int flags,
                           int fd, off64_t offset)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        return UNIFYFS_WRAP(mmap)(addr, length, prot, flags, fd, (off_t)offset);
    } else {
        MAP_OR_FAIL(mmap64);
        void* ret = UNIFYFS_REAL(mmap64)(addr, length, prot, flags, fd, offset);
        return ret;
    }
}

int UNIFYFS_WRAP(close)(int fd)
{
    /* check whether we should intercept this file descriptor */
    int origfd = fd;
    if (unifyfs_intercept_fd(&fd)) {
        LOGDBG("closing fd %d", fd);

        /* TODO: what to do if underlying file has been deleted? */

        /* check that fd is actually in use */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            errno = EBADF;
            return -1;
        }

        /* get file descriptor for this file */
        unifyfs_fd_t* filedesc = unifyfs_get_filedesc_from_fd(fd);
        if (filedesc == NULL) {
            errno = EBADF;
            return -1;
        }

        /* if file was opened for writing, fsync it */
        if (filedesc->write) {
            UNIFYFS_WRAP(fsync)(origfd);
        }

        /* close the file id */
        int close_rc = unifyfs_fid_close(fid);
        if (close_rc != UNIFYFS_SUCCESS) {
            errno = EIO;
            return -1;
        }

        /* reinitialize file descriptor to indicate that
         * it is no longer associated with a file,
         * not technically needed but may help catch bugs */
        unifyfs_fd_init(fd);

        /* add file descriptor back to free stack */
        unifyfs_stack_push(unifyfs_fd_stack, fd);

        return 0;
    } else {
        MAP_OR_FAIL(close);
        int ret = UNIFYFS_REAL(close)(fd);
        return ret;
    }
}

/* Helper function used by fchmod() and chmod() */
static int __chmod(int fid, mode_t mode)
{
    int ret;

    /* get path for printing debug messages */
    const char* path = unifyfs_path_from_fid(fid);

    /* lookup metadata for this file */
    unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
    if (!meta) {
        LOGDBG("chmod: %s no metadata info", path);
        errno = ENOENT;
        return -1;
    }

    /* Once a file is laminated, you can't modify it in any way */
    if (meta->is_laminated) {
        LOGDBG("chmod: %s is already laminated", path);
        errno = EROFS;
        return -1;
    }

    /* found file, and it's not yet laminated,
     * get the global file id */
    int gfid = unifyfs_gfid_from_fid(fid);

    /* TODO: need to fetch global metadata in case
     * another process has changed it */

    /*
     * If the chmod clears all the existing write bits, then it's a laminate.
     *
     * meta->mode & 0222                  Was at least one write bit set before?
     * ((meta->mode & 0222) & mode) == 0  Will all the write bits be cleared?
     */
    if ((meta->mode & 0222) &&
        (((meta->mode & 0222) & mode) == 0)) {

        /*
         * We're laminating. Calculate the file size so we can cache it
         * (both locally and on the server).
         */
        ret = invoke_client_filesize_rpc(gfid, &meta->global_size);
        if (ret) {
            LOGERR("chmod: couldn't get the global file size on laminate");
            errno = EIO;
            return -1;
        }

        /* record locally that this file is now laminated */
        meta->is_laminated = 1;
    }

    /* Clear out our old permission bits, and set the new ones in */
    meta->mode = meta->mode & ~0777;
    meta->mode = meta->mode | mode;

    /* update the global meta data to reflect new permissions,
     * size, and laminated flag */
    ret = unifyfs_set_global_file_meta_from_fid(fid);
    if (ret) {
        LOGERR("chmod: can't set global meta entry for %s (fid:%d)",
               path, fid);
        errno = EIO;
        return -1;
    }

    return 0;
}

int UNIFYFS_WRAP(fchmod)(int fd, mode_t mode)
{
    /* check whether we should intercept this file descriptor */
    if (unifyfs_intercept_fd(&fd)) {
        /* TODO: what to do if underlying file has been deleted? */

        /* check that fd is actually in use */
        int fid = unifyfs_get_fid_from_fd(fd);
        if (fid < 0) {
            errno = EBADF;
            return -1;
        }

        LOGDBG("fchmod: setting fd %d to %o", fd, mode);
        return __chmod(fid, mode);
    } else {
        MAP_OR_FAIL(fchmod);
        int ret = UNIFYFS_REAL(fchmod)(fd, mode);
        return ret;
    }
}

int UNIFYFS_WRAP(chmod)(const char* path, mode_t mode)
{
    /* determine whether we should intercept this path */
    if (unifyfs_intercept_path(path)) {
        /* check if path exists */
        int fid = unifyfs_get_fid_from_path(path);
        if (fid < 0) {
            LOGDBG("chmod: unifyfs_get_id_from path failed, returning -1, %s",
                   path);
            errno = ENOENT;
            return -1;
        }

        LOGDBG("chmod: setting %s to %o", path, mode);
        return __chmod(fid, mode);
    } else {
        MAP_OR_FAIL(chmod);
        int ret = UNIFYFS_REAL(chmod)(path, mode);
        return ret;
    }
}
