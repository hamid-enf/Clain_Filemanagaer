# FILE3-01: "delete file" removes a directory.
#
# The adapter contract (include/fm_filesystem.h) says
#     delete_file: files only        rmdir: empty dirs only
# but two adapters are one-line wrappers over an API that removes both
# kinds of entry:
#     LittleFS  lfs_fs_delete -> lfs_remove()   (removes an empty dir)
#     POSIX     posix_delete  -> remove(2)      (removes an empty dir)
# so File_Delete("/dir") on an empty directory returned FM_OK and the
# directory was gone -- no error, no way for the caller to notice.  Both
# directions are fixed: the file entry point refuses a directory
# (FM_ERR_IS_DIRECTORY) and the directory entry point refuses a file
# (FM_ERR_NOT_DIRECTORY), so a mixed-up caller is told which one it has
# instead of losing it.
import io

# ------------------------------------------------------------------ #
# 1. LittleFS                                                        #
# ------------------------------------------------------------------ #
p = 'backends/fm_littlefs.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

old = """    lfs_path(path, lpath, sizeof(lpath));
    return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
}
"""
new = """    lfs_path(path, lpath, sizeof(lpath));
    /* lfs_remove() removes an empty directory as happily as a file, so
     * "delete file" used to delete a directory -- silently, and with
     * FM_OK.  The adapter contract is files only (fm_filesystem.h:
     * "delete_file: files only"); a directory is refused here, and the
     * directory API (lfs_fs_rmdir) does the rest. */
    {
        struct lfs_info info;
        int stat_err = lfs_stat(&ctx->lfs, lpath, &info);

        if (stat_err != LFS_ERR_OK)
        {
            return lfs_map_err(stat_err);
        }
        if (info.type == LFS_TYPE_DIR)
        {
            return FM_ERR_IS_DIRECTORY;
        }
    }
    return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
}
"""
old = old.replace('\n', '\r\n')
new = new.replace('\n', '\r\n')
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)

old2 = """static FM_Result lfs_fs_rmdir(FM_FS *fs, const char *path)
{
    /* littlefs lfs_remove removes empty dirs and files alike. */
    return lfs_fs_delete(fs, path);
}
""".replace('\n', '\r\n')
new2 = """static FM_Result lfs_fs_rmdir(FM_FS *fs, const char *path)
{
    LittleFS_Ctx *ctx = lfs_ctx(fs);
    char lpath[FM_PATH_MAX];
    struct lfs_info info;
    int err;

    if (path == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    lfs_path(path, lpath, sizeof(lpath));
    /* the mirror image of the check above: a file is not a directory */
    err = lfs_stat(&ctx->lfs, lpath, &info);
    if (err != LFS_ERR_OK)
    {
        return lfs_map_err(err);
    }
    if (info.type != LFS_TYPE_DIR)
    {
        return FM_ERR_NOT_DIRECTORY;
    }
    return lfs_map_err(lfs_remove(&ctx->lfs, lpath));
}
""".replace('\n', '\r\n')
assert s.count(old2) == 1, s.count(old2)
s = s.replace(old2, new2)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('f3_01: patched %s' % p)

# ------------------------------------------------------------------ #
# 2. POSIX -- the same defect, one line of remove(2)                 #
# ------------------------------------------------------------------ #
p = 'backends/fm_posix_fs.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

old = """static FM_Result posix_delete(FM_FS *fs, const char *path)
{
    path = strip_slash(path);
    if (remove(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}
"""
new = """static FM_Result posix_delete(FM_FS *fs, const char *path)
{
    struct stat st;

    path = strip_slash(path);
    /* remove(2) unlinks a file and removes an empty directory alike, so
     * File_Delete(\"/dir\") used to succeed and take the directory with
     * it.  The adapter contract is files only; a directory is refused
     * here and posix_rmdir() handles it. */
    if (stat(path, &st) < 0) { return sys2fm(errno); }
    if (S_ISDIR(st.st_mode)) { return FM_ERR_IS_DIRECTORY; }
    if (remove(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}
"""
old = old.replace('\n', '\r\n')
new = new.replace('\n', '\r\n')
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)

old2 = """static FM_Result posix_rmdir(FM_FS *fs, const char *path)
{
    path = strip_slash(path);
    if (rmdir(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}
""".replace('\n', '\r\n')
new2 = """static FM_Result posix_rmdir(FM_FS *fs, const char *path)
{
    struct stat st;

    path = strip_slash(path);
    /* rmdir(2) already refuses a file with ENOTDIR, but the error is
     * mapped after the fact; check first so the answer does not depend
     * on the platform's errno mapping. */
    if (stat(path, &st) < 0) { return sys2fm(errno); }
    if (!S_ISDIR(st.st_mode)) { return FM_ERR_NOT_DIRECTORY; }
    if (rmdir(path) < 0) { return sys2fm(errno); }
    (void)fs;
    return FM_OK;
}
""".replace('\n', '\r\n')
assert s.count(old2) == 1, s.count(old2)
s = s.replace(old2, new2)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('f3_01: patched %s' % p)
