/*
 * FileManager - POSIX filesystem adapter (host testing only)
 *
 * Implements the FM_FSOps function table on top of the host OS
 * file system.  Only used when FM_USE_POSIX == 1 (desktop build).
 */

#include "fm_internal.h"

#if FM_USE_POSIX

#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

/* Pre-allocate storage for the posix FS handle (must fit into
 * FM_FS_HANDLE_SIZE bytes). */
typedef struct
{
    int fd;
    uint8_t used;
} PosixFile;

typedef struct
{
    DIR *dir;
    char dir_path[FM_PATH_MAX];
    uint8_t used;
} PosixDir;

typedef char posix_file_handle_must_fit[
    (sizeof(PosixFile) <= FM_FS_HANDLE_SIZE) ? 1 : -1];
typedef char posix_dir_handle_must_fit[
    (sizeof(PosixDir) <= FM_FS_DIRHANDLE_SIZE) ? 1 : -1];

static FM_Result sys2fm(int sys_err)
{
    switch (sys_err)
    {
    case ENOENT:  return FM_ERR_NOT_FOUND;
    case EACCES:  return FM_ERR_ACCESS;
    case EEXIST:  return FM_ERR_EXISTS;
    case ENOSPC:  return FM_ERR_NO_SPACE;
    case ENOTDIR: return FM_ERR_NOT_DIRECTORY;
    case EISDIR:  return FM_ERR_IS_DIRECTORY;
    case ENOTEMPTY: return FM_ERR_NOT_EMPTY;
    case EBUSY:   return FM_ERR_BUSY;
    case EROFS:   return FM_ERR_READ_ONLY;
    case EINVAL:  return FM_ERR_INVALID_ARGUMENT;  /* e.g. seek before 0 */
    default:      return FM_ERR_IO;
    }
}

static FM_Result posix_mount(FM_FS *fs, FM_Storage *storage)
{
    (void)fs;
    (void)storage;
    return FM_OK;
}

static FM_Result posix_unmount(FM_FS *fs)
{
    (void)fs;
    return FM_OK;
}

static FM_Result posix_format(FM_FS *fs)
{
    (void)fs;
    return FM_ERR_UNSUPPORTED;
}

static FM_Result posix_check(FM_FS *fs)
{
    (void)fs;
    return FM_OK;
}

static int open_flags(uint32_t mode)
{
    int flags = 0;

    if (mode & FM_OPEN_READ)
    {
        if (mode & FM_OPEN_WRITE) { flags = O_RDWR; }
        else                      { flags = O_RDONLY; }
    }
    else if (mode & FM_OPEN_WRITE)
    {
        flags = O_WRONLY;
    }
    if (mode & FM_OPEN_CREATE)   { flags |= O_CREAT; }
    if (mode & FM_OPEN_EXCLUSIVE) { flags |= O_EXCL; }
    if (mode & FM_OPEN_TRUNCATE) { flags |= O_TRUNC; }
    if (mode & FM_OPEN_APPEND)   { flags |= O_APPEND; }
    return flags;
}

/* Strip leading slashes from the path.  POSIX root-relative means we
 * strip them all (file_manager paths are always absolute). */
static const char *strip_slash(const char *path)
{
    while (*path == '/') path++;
    if (*path == '\0') return ".";
    return path;
}

static FM_Result posix_file_open(FM_FS *fs, void *fh, const char *path,
                                 uint32_t mode)
{
    PosixFile *pf = (PosixFile *)fh;
    int fd;

    path = strip_slash(path);
    fd = open(path, open_flags(mode), 0644);
    if (fd < 0) { return sys2fm(errno); }
    /* open(2) opens a directory read-only without complaint; the other
     * adapters answer FM_ERR_IS_DIRECTORY and so must this one, or
     * File_Open hands out a handle every read of which fails. */
    {
        struct stat sb;

        if (fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode))
        {
            (void)close(fd);
            return FM_ERR_IS_DIRECTORY;
        }
    }
    pf->fd = fd;
    pf->used = 1;
    (void)fs;
    return FM_OK;
}

static FM_Result posix_file_close(FM_FS *fs, void *fh)
{
    PosixFile *pf = (PosixFile *)fh;
    (void)fs;
    if (!pf->used) { return FM_ERR_NOT_OPEN; }
    close(pf->fd);
    pf->used = 0;
    return FM_OK;
}

static FM_Result posix_file_read(FM_FS *fs, void *fh, void *buf,
                                 uint32_t len, uint32_t *out_read)
{
    PosixFile *pf = (PosixFile *)fh;
    ssize_t r;

    (void)fs;
    r = (ssize_t)read(pf->fd, buf, len);
    if (r < 0) { return sys2fm(errno); }
    if (out_read) { *out_read = (uint32_t)r; }
    if (r == 0) { return FM_ERR_EOF; }
    return FM_OK;
}

static FM_Result posix_file_write(FM_FS *fs, void *fh, const void *buf,
                                  uint32_t len, uint32_t *written)
{
    PosixFile *pf = (PosixFile *)fh;
    ssize_t w;

    (void)fs;
    w = (ssize_t)write(pf->fd, buf, len);
    if (w < 0) { return sys2fm(errno); }
    if (written) { *written = (uint32_t)w; }
    return FM_OK;
}

static FM_Result posix_file_seek(FM_FS *fs, void *fh, int64_t offset,
                                 FM_SeekOrigin origin)
{
    PosixFile *pf = (PosixFile *)fh;
    int whence;

    (void)fs;
    switch (origin)
    {
    case FM_SEEK_SET: whence = SEEK_SET; break;
    case FM_SEEK_CUR: whence = SEEK_CUR; break;
    case FM_SEEK_END: whence = SEEK_END; break;
    default: return FM_ERR_INVALID_ARGUMENT;
    }
    if (lseek(pf->fd, offset, whence) < 0) { return sys2fm(errno); }
    return FM_OK;
}

static FM_Result posix_file_tell(FM_FS *fs, void *fh, uint64_t *pos)
{
    PosixFile *pf = (PosixFile *)fh;
    off_t p;

    (void)fs;
    p = lseek(pf->fd, 0, SEEK_CUR);
    if (p < 0) { return sys2fm(errno); }
    *pos = (uint64_t)p;
    return FM_OK;
}

static FM_Result posix_file_size(FM_FS *fs, void *fh, uint64_t *size)
{
    PosixFile *pf = (PosixFile *)fh;
    struct stat st;

    (void)fs;
    if (fstat(pf->fd, &st) < 0) { return sys2fm(errno); }
    *size = (uint64_t)st.st_size;
    return FM_OK;
}

static FM_Result posix_file_truncate(FM_FS *fs, void *fh, uint64_t size)
{
    PosixFile *pf = (PosixFile *)fh;
    (void)fs;
    if (ftruncate(pf->fd, (off_t)size) < 0) { return sys2fm(errno); }
    return FM_OK;
}

static FM_Result posix_file_flush(FM_FS *fs, void *fh)
{
    PosixFile *pf = (PosixFile *)fh;
    (void)fs;
    if (fsync(pf->fd) < 0) { return FM_ERR_IO; }
    return FM_OK;
}

static FM_Result posix_stat(FM_FS *fs, const char *path, FM_FileInfo *info)
{
    struct stat st;

    path = strip_slash(path);
    if (stat(path, &st) < 0) { return sys2fm(errno); }
    memset(info, 0, sizeof(*info));
    {
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;

        if (strlen(name) >= sizeof(info->name))
        {
            return FM_ERR_TOO_LARGE;
        }
        (void)strcpy(info->name, name);
    }
    info->type = S_ISDIR(st.st_mode) ? FM_ITEM_DIR : FM_ITEM_FILE;
    info->size = (info->type == FM_ITEM_DIR) ? 0u : (uint64_t)st.st_size;
    info->modified = (uint64_t)st.st_mtime;
    (void)fs;
    return FM_OK;
}

static FM_Result posix_delete(FM_FS *fs, const char *path)
{
    path = strip_slash(path);
    if (remove(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}

/* link(2) creates the destination atomically only when absent. The source
 * is a staged regular file, on the same filesystem. If unlink fails the
 * destination is committed but the old name remains. */
static FM_Result posix_rename_noreplace(FM_FS *fs, const char *old_path,
                                         const char *new_path)
{
    const char *old_name = strip_slash(old_path);
    const char *new_name = strip_slash(new_path);
    (void)fs;
    if (link(old_name, new_name) < 0) return sys2fm(errno);
    if (unlink(old_name) < 0) return FM_ERR_CLEANUP;
    return FM_OK;
}

static FM_Result posix_rename(FM_FS *fs, const char *old_path,
                              const char *new_path)
{
    char ob[FM_PATH_MAX];
    char nb[FM_PATH_MAX];
    const char *o;
    const char *n;

    if (old_path == NULL || new_path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    o = strip_slash(old_path);
    n = strip_slash(new_path);
    if (strlen(o) >= sizeof(ob) || strlen(n) >= sizeof(nb))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(ob, o);
    (void)strcpy(nb, n);
    if (rename(ob, nb) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}

static FM_Result posix_mkdir(FM_FS *fs, const char *path)
{
    const char *p;
    struct stat st;

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    p = strip_slash(path);
    if (mkdir(p, 0755) == 0)
    {
        (void)fs;
        return FM_OK;
    }
    if (errno != EEXIST)
    {
        return sys2fm(errno);
    }
    if (stat(p, &st) < 0)
    {
        return sys2fm(errno);
    }
    (void)fs;
    return S_ISDIR(st.st_mode) ? FM_ERR_EXISTS : FM_ERR_NOT_DIRECTORY;
}

static FM_Result posix_rmdir(FM_FS *fs, const char *path)
{
    path = strip_slash(path);
    if (rmdir(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}

static FM_Result posix_dir_open(FM_FS *fs, void *dh, const char *path)
{
    PosixDir *pd = (PosixDir *)dh;
    const char *p;

    if (dh == NULL || path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    p = strip_slash(path);
    if (strlen(p) >= sizeof(pd->dir_path))
    {
        return FM_ERR_TOO_LARGE;
    }
    pd->dir = opendir(p);
    if (pd->dir == NULL) { return sys2fm(errno); }
    pd->used = 1;
    (void)strcpy(pd->dir_path, p);
    (void)fs;
    return FM_OK;
}

static FM_Result posix_dir_close(FM_FS *fs, void *dh)
{
    PosixDir *pd = (PosixDir *)dh;

    if (pd == NULL || !pd->used) { return FM_ERR_NOT_OPEN; }
    if (closedir(pd->dir) != 0)
    {
        pd->used = 0;
        return FM_ERR_IO;
    }
    pd->used = 0;
    (void)fs;
    return FM_OK;
}

static FM_Result posix_dir_read(FM_FS *fs, void *dh, FM_DirEntry *entry)
{
    PosixDir *pd = (PosixDir *)dh;
    struct dirent *de;

    if (pd == NULL || entry == NULL || !pd->used)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)fs;
    for (;;)
    {
        struct stat st;
        char full[FM_PATH_MAX];
        size_t dlen;
        size_t nlen;

        de = readdir(pd->dir);
        if (de == NULL) { return FM_ERR_EOF; }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        {
            continue;
        }
        dlen = strlen(pd->dir_path);
        nlen = strlen(de->d_name);
        if (dlen + nlen + 2 > sizeof(full))
        {
            return FM_ERR_TOO_LARGE;
        }
        (void)memcpy(full, pd->dir_path, dlen);
        if (dlen > 0 && full[dlen - 1] != '/')
        {
            full[dlen++] = '/';
        }
        (void)memcpy(full + dlen, de->d_name, nlen + 1);
        if (lstat(full, &st) != 0)
        {
            return sys2fm(errno);
        }

        if (nlen >= sizeof(entry->name))
        {
            return FM_ERR_TOO_LARGE;
        }
        memset(entry, 0, sizeof(*entry));
        (void)memcpy(entry->name, de->d_name, nlen + 1);
        entry->type = S_ISDIR(st.st_mode) ? FM_ITEM_DIR : FM_ITEM_FILE;
        entry->size = (entry->type == FM_ITEM_DIR) ? 0u :
                      (uint64_t)st.st_size;
        entry->timestamp = (uint32_t)st.st_mtime;
        if ((st.st_mode & S_IWUSR) == 0)
        {
            entry->attributes = FM_ATTR_READ_ONLY;
        }
        return FM_OK;
    }
}

static FM_Result posix_get_free(FM_FS *fs, uint64_t *free, uint64_t *total)
{
    struct statvfs v;
    const char *path = ".";

    (void)fs;
    if (free == NULL || total == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (statvfs(path, &v) != 0)
    {
        *free = 0;
        *total = 0;
        return sys2fm(errno);
    }
    if (v.f_frsize != 0 &&
        (uint64_t)v.f_blocks > UINT64_MAX / (uint64_t)v.f_frsize)
    {
        return FM_ERR_TOO_LARGE;
    }
    *total = (uint64_t)v.f_blocks * (uint64_t)v.f_frsize;
    if (v.f_frsize != 0 &&
        (uint64_t)v.f_bavail > UINT64_MAX / (uint64_t)v.f_frsize)
    {
        return FM_ERR_TOO_LARGE;
    }
    *free = (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
    return FM_OK;
}

static FM_Result posix_get_type(FM_FS *fs, char *type, size_t type_size)
{
    if (type == NULL || type_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    (void)fs;
    if (type_size < sizeof("POSIX"))
    {
        return FM_ERR_TOO_LARGE;
    }
    (void)strcpy(type, "POSIX");
    return FM_OK;
}

FM_Result PosixFS_Init(FM_FS *fs)
{
    if (fs == NULL) { return FM_ERR_INVALID_ARGUMENT; }
    memset(fs, 0, sizeof(*fs));
    fs->capabilities = FM_FS_CAP_EXCLUSIVE_CREATE;
    (void)strncpy(fs->name, "POSIX", sizeof(fs->name) - 1);
    fs->mounted = FM_FALSE;
    fs->storage = NULL;
    fs->ops.mount = posix_mount;
    fs->ops.unmount = posix_unmount;
    fs->ops.format = posix_format;
    fs->ops.check = posix_check;
    fs->ops.file_open = posix_file_open;
    fs->ops.file_close = posix_file_close;
    fs->ops.file_read = posix_file_read;
    fs->ops.file_write = posix_file_write;
    fs->ops.file_seek = posix_file_seek;
    fs->ops.file_tell = posix_file_tell;
    fs->ops.file_size = posix_file_size;
    fs->ops.file_truncate = posix_file_truncate;
    fs->ops.file_flush = posix_file_flush;
    fs->ops.stat = posix_stat;
    fs->ops.delete_file = posix_delete;
    fs->ops.rename = posix_rename;
    fs->ops.rename_noreplace = posix_rename_noreplace;
    fs->ops.mkdir = posix_mkdir;
    fs->ops.rmdir = posix_rmdir;
    fs->ops.dir_open = posix_dir_open;
    fs->ops.dir_close = posix_dir_close;
    fs->ops.dir_read = posix_dir_read;
    fs->ops.get_free = posix_get_free;
    fs->ops.get_type = posix_get_type;
    return FM_OK;
}
#endif /* FM_USE_POSIX */
