/*
 * ff_mock.c - FatFS API simulator (host tests only).
 * See ff.h for details. Not a real FAT - file level simulation.
 */
#include "ff.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MOCK_MAX_FILES 32
#define MOCK_MAX_DATA  (64 * 1024)

typedef struct {
    uint8_t used;
    uint8_t is_dir;
    char    name[256];
    uint8_t attrib;
    size_t  len;
    size_t  cap;
    uint8_t data[MOCK_MAX_DATA];
} MockFile;

static FATFS *g_vol[FF_VOLUMES];
static MockFile g_files[MOCK_MAX_FILES];

void ff_mock_reset(void)
{
    memset(g_files, 0, sizeof(g_files));
    memset(g_vol, 0, sizeof(g_vol));
}

int ff_mock_file_count(void)
{
    int i, n = 0;
    for (i = 0; i < MOCK_MAX_FILES; i++) n += g_files[i].used ? 1 : 0;
    return n;
}

/* ---- helpers ---- */
static int name_ok(const char *path)
{
    /* must be "0:/..." (only volume 0 exists) */
    if (path == NULL) return 0;
    if (path[0] != '0' || path[1] != ':') return 0;
    if (path[2] != '/') return 0;
    return 1;
}

static const char *strip(const char *path)
{
    const char *p = path + 2;
    while (*p == '/') p++;
    return p;
}

static MockFile *find_file(const char *path)
{
    int i;
    const char *name;
    if (!name_ok(path)) return NULL;
    name = strip(path);
    if (name[0] == '\0') return NULL;
    for (i = 0; i < MOCK_MAX_FILES; i++)
        if (g_files[i].used && strcmp(g_files[i].name, name) == 0)
            return &g_files[i];
    return NULL;
}

static MockFile *alloc_file(const char *path, uint8_t is_dir)
{
    int i;
    const char *name;
    if (!name_ok(path)) return NULL;
    name = strip(path);
    if (name[0] == '\0' || strlen(name) >= sizeof(g_files[0].name)) return NULL;
    for (i = 0; i < MOCK_MAX_FILES; i++) {
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            g_files[i].used = 1;
            g_files[i].is_dir = is_dir;
            strcpy(g_files[i].name, name);
            g_files[i].attrib = is_dir ? AM_DIR : AM_ARC;
            return &g_files[i];
        }
    }
    return NULL;
}

/* Simulate find_volume: touching the disk validates the FS object.
 * fs_type stays set even after f_mount(NULL) deregistration (like FatFS,
 * the object is not cleared) - this is what triggers the adapter's
 * g_fatfs slot-leak reproduction. */
static FRESULT volume_ok(FATFS **out)
{
    FATFS *fs = g_vol[0];
    if (fs == NULL) return FR_NOT_ENABLED;      /* not registered      */
    if (fs->fs_type == 0)                       /* lazy validation     */
    {
        fs->fs_type = FS_FAT32;
        fs->csize = 64;                         /* 32 KB clusters      */
        fs->n_fatent = 100002;
    }
    if (out) *out = fs;
    return FR_OK;
}

/* ---- mount ---- */
FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{
    (void)opt;
    if (path == NULL || path[0] == '\0') return FR_INVALID_NAME;
    if (path[0] != '0' || path[1] != ':') return FR_INVALID_DRIVE;
    g_vol[0] = fs;                              /* NULL = deregister  */
    return FR_OK;
}

/* ---- file ops ---- */
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    MockFile *f;
    if (fp == NULL) return FR_INVALID_OBJECT;
    memset(fp, 0, sizeof(*fp));
    if ((mode & (FA_READ | FA_WRITE)) == 0) return FR_INVALID_PARAMETER;
    {
        FRESULT fr = volume_ok(&fp->obj_fs);
        if (fr != FR_OK) return fr;
    }
    f = find_file(path);
    if (f != NULL && f->is_dir) return FR_NO_PATH;

    if (mode & FA_CREATE_ALWAYS) {
        if (f == NULL) f = alloc_file(path, 0);
        if (f == NULL) return FR_DENIED;
        f->len = 0;
    } else if (mode & (FA_CREATE_NEW)) {
        if (f != NULL) return FR_EXIST;
        f = alloc_file(path, 0);
        if (f == NULL) return FR_DENIED;
    } else if (mode & FA_OPEN_ALWAYS) {
        if (f == NULL) f = alloc_file(path, 0);
        if (f == NULL) return FR_DENIED;
    } else {                                    /* FA_OPEN_EXISTING */
        if (f == NULL) return FR_NO_FILE;
    }

    fp->mo_file = (int)(f - g_files);
    fp->fmode = mode;
    fp->is_open = 1;
    if (mode & FA_OPEN_APPEND) fp->fptr = (FSIZE_t)f->len;
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    fp->is_open = 0;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    MockFile *f;
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    if (br) *br = 0;
    if (!(fp->fmode & FA_READ)) return FR_DENIED;
    f = &g_files[fp->mo_file];
    if (fp->fptr >= (FSIZE_t)f->len) return FR_OK;   /* EOF: *br = 0 */
    {
        FSIZE_t avail = (FSIZE_t)f->len - fp->fptr;
        UINT n = (UINT)((avail < (FSIZE_t)btr) ? avail : (FSIZE_t)btr);
        memcpy(buff, f->data + (size_t)fp->fptr, n);
        fp->fptr += n;
        if (br) *br = n;
    }
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    MockFile *f;
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    if (bw) *bw = 0;
    if (!(fp->fmode & FA_WRITE)) return FR_DENIED;
    f = &g_files[fp->mo_file];
    if ((size_t)fp->fptr + btw > f->cap) {
        size_t need = (size_t)fp->fptr + btw;
        if (need > MOCK_MAX_DATA) return FR_DENIED;  /* disk full */
        f->cap = need;
    }
    memcpy(f->data + (size_t)fp->fptr, buff, btw);
    fp->fptr += btw;
    if ((size_t)fp->fptr > f->len) f->len = (size_t)fp->fptr;
    if (bw) *bw = btw;
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT f_truncate(FIL *fp)
{
    MockFile *f;
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    if (!(fp->fmode & FA_WRITE)) return FR_DENIED;
    f = &g_files[fp->mo_file];
    f->len = (size_t)fp->fptr;
    return FR_OK;
}

FRESULT f_sync(FIL *fp)
{
    if (fp == NULL || !fp->is_open) return FR_INVALID_OBJECT;
    return FR_OK;
}

FSIZE_t f_size(FIL *fp)
{
    assert(fp != NULL && fp->is_open);
    return (FSIZE_t)g_files[fp->mo_file].len;
}

FSIZE_t f_tell(FIL *fp)
{
    assert(fp != NULL && fp->is_open);
    return fp->fptr;
}

/* ---- directory level ---- */
FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    MockFile *f;
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    f = find_file(path);
    if (f == NULL) return FR_NO_FILE;
    if (fno) {
        memset(fno, 0, sizeof(*fno));
        fno->fsize = f->is_dir ? 0 : (FSIZE_t)f->len;
        fno->fattrib = f->attrib;
        strcpy(fno->fname, f->name);
    }
    return FR_OK;
}

FRESULT f_unlink(const TCHAR *path)
{
    MockFile *f;
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    f = find_file(path);
    if (f == NULL) return FR_NO_FILE;
    if (f->is_dir) {
        int i, prefix = (int)strlen(f->name);
        for (i = 0; i < MOCK_MAX_FILES; i++)
            if (g_files[i].used && strncmp(g_files[i].name, f->name,
                                           (size_t)prefix) == 0 &&
                g_files[i].name[prefix] == '/')
                return FR_DENIED;               /* dir not empty */
    }
    f->used = 0;
    return FR_OK;
}

FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new)
{
    MockFile *src, *dst;
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    src = find_file(path_old);
    if (src == NULL) return FR_NO_FILE;
    dst = find_file(path_new);
    if (dst != NULL) return FR_EXIST;
    {
        const char *nname = strip(path_new);
        if (strlen(nname) >= sizeof(src->name)) return FR_INVALID_NAME;
        strcpy(src->name, nname);
    }
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path)
{
    MockFile *d;
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    if (find_file(path) != NULL) return FR_EXIST;
    d = alloc_file(path, 1);
    if (d == NULL) return FR_DENIED;
    return FR_OK;
}

FRESULT f_rmdir(const TCHAR *path)
{
    MockFile *d = find_file(path);
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    if (d == NULL) return FR_NO_PATH;
    if (!d->is_dir) return FR_INVALID_NAME;
    return f_unlink(path);
}

FRESULT f_opendir(DIR *dp, const TCHAR *path)
{
    FRESULT fr = volume_ok(NULL);
    if (fr != FR_OK) return fr;
    if (dp == NULL) return FR_INVALID_OBJECT;
    memset(dp, 0, sizeof(*dp));
    {
        const char *p = path + 2;
        while (*p == '/') p++;
        if (*p != '\0') {
            MockFile *d = find_file(path);
            if (d == NULL) return FR_NO_PATH;
            if (!d->is_dir) return FR_INVALID_NAME;
        }
        strcpy(dp->path, p);                    /* "" = root */
    }
    dp->cursor = -1;                            /* emit "." ".." first */
    dp->is_open = 1;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    int i;
    if (dp == NULL || !dp->is_open) return FR_INVALID_OBJECT;
    if (fno) memset(fno, 0, sizeof(*fno));

    /* FAT reports "." and ".." first - the adapter must skip them. */
    if (dp->cursor == -1) {
        dp->cursor = 0;
        if (fno) strcpy(fno->fname, ".");
        return FR_OK;
    }
    if (dp->cursor == 0) {
        dp->cursor = 1;
        if (fno) strcpy(fno->fname, "..");
        return FR_OK;
    }

    for (i = dp->cursor - 1; i < MOCK_MAX_FILES; i++) {
        MockFile *f = &g_files[i];
        const char *rel;
        if (!f->used) continue;
        /* direct children only */
        if (dp->path[0] != '\0') {
            size_t plen = strlen(dp->path);
            if (strncmp(f->name, dp->path, plen) != 0) continue;
            if (f->name[plen] != '/') continue;
            rel = f->name + plen + 1;
        } else {
            rel = f->name;
        }
        if (strchr(rel, '/') != NULL) continue;
        dp->cursor = i + 2;
        if (fno) {
            fno->fsize = f->is_dir ? 0 : (FSIZE_t)f->len;
            fno->fattrib = f->attrib;
            strcpy(fno->fname, rel);
        }
        return FR_OK;
    }
    /* end of directory: FatFS sets fname[0] = 0 */
    if (fno) fno->fname[0] = '\0';
    return FR_OK;
}

FRESULT f_closedir(DIR *dp)
{
    if (dp == NULL || !dp->is_open) return FR_INVALID_OBJECT;
    dp->is_open = 0;
    return FR_OK;
}

FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
{
    FRESULT fr = volume_ok(fatfs);
    if (fr != FR_OK) return fr;
    /* FatFS accepts both "0:" and "0:/" here */
    if (path == NULL || path[0] != '0' || path[1] != ':')
        return FR_INVALID_DRIVE;
    if (nclst) *nclst = 100000;                 /* ~50 MB free */
    return FR_OK;
}

#ifdef MOCK_FATFS_MODERN
FRESULT f_mkfs(const TCHAR *path, const MKFS_PARM *opt, void *work, UINT len)
{
    (void)opt; (void)work; (void)len;
    if (path == NULL) return FR_INVALID_NAME;
    return FR_OK;
}
#elif defined(MOCK_FATFS_LEGACY)
FRESULT f_mkfs(const TCHAR *path, BYTE opt, DWORD au, void *work)
{
    (void)opt; (void)au; (void)work;
    if (path == NULL) return FR_INVALID_NAME;
    return FR_OK;
}
#else
FRESULT f_mkfs(const TCHAR *path, BYTE opt, DWORD au, void *work, UINT len)
{
    (void)opt; (void)au; (void)work; (void)len;
    if (path == NULL) return FR_INVALID_NAME;
    return FR_OK;
}
#endif
