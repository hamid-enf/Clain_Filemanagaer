# FILE3-05: FS_Mount() publishes an adapter that cannot do file I/O.
#
# An FM_FS that was never passed to FMFS_Init / LittleFS_Init / FatFS_Init /
# PosixFS_Init is all zeros.  FS_Mount() finds no mount hook to call, so
# nothing fails: the mount is published with FM_OK and the first
# File_Open() jumps through ops.file_open == NULL -- a hard fault on a
# target, a SEGV at address 0 on the host.
#
# FS_Register() has refused exactly these three ops since it was written;
# FS_Mount() does not go through the registry, so it never checked.  The
# check goes at the end of mount_bind(), after the adapter's own mount
# hook has run, so:
#   * an adapter whose mount hook fails still gets its own error back
#     (that is a diagnosis, not a crash),
#   * an adapter with no mount hook at all (FMFS/LittleFS style tables
#     built without one) is still accepted when it can do file I/O,
#   * and the all-zero table is refused before it is published.
import io

p = 'src/fm_filesystem.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()

old = """    fs->storage = storage;
    if (fs->ops.mount != NULL)
    {
        return fs->ops.mount(fs, storage);
    }
    return (storage == NULL) ? FM_ERR_NOT_READY : FM_OK;
}
"""
new = """    fs->storage = storage;
    if (fs->ops.mount != NULL)
    {
        FM_Result r = fs->ops.mount(fs, storage);

        if (r != FM_OK)
        {
            return r;               /* the adapter's own diagnosis wins */
        }
    }
    else if (storage == NULL)
    {
        return FM_ERR_NOT_READY;
    }

    /* A table that got this far without a file-open, file-read and
     * file-write hook is an FM_FS that was never handed to its adapter's
     * *_Init() (all zeros behind no mount hook) or one that is only half
     * filled in.  Publishing it makes the first File_Open() call a NULL
     * function pointer -- a hard fault.  FS_Register() has always refused
     * exactly these three ops; a mount does not go through the registry,
     * so it checks them here. */
    if (fs->ops.file_open == NULL || fs->ops.file_read == NULL ||
        fs->ops.file_write == NULL)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    return FM_OK;
}
"""

old = old.replace('\n', '\r\n')
new = new.replace('\n', '\r\n')

if old not in s:
    raise SystemExit('f3_05: needle not found in %s' % p)
if s.count(old) != 1:
    raise SystemExit('f3_05: needle is not unique in %s' % p)

io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
print('f3_05: patched %s' % p)
