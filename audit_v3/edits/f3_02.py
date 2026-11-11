import io
p = 'src/fm_fmfs.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()
old = """            if (ctx->entries[old_index].type != FM_ITEM_DIR)
            {
                return FM_ERR_NOT_DIRECTORY;
            }
        }
        else
        {
            if (ctx->entries[old_index].type == FM_ITEM_DIR)
            {
                return FM_ERR_NOT_DIRECTORY;
            }
        }
""".replace('\n', '\r\n')
new = """            if (ctx->entries[old_index].type != FM_ITEM_DIR)
            {
                /* the DESTINATION is a directory and the source is not:
                 * that is "is a directory", not "not a directory" (the
                 * two other backends say FM_ERR_IS_DIRECTORY here) */
                return FM_ERR_IS_DIRECTORY;
            }
        }
        else
        {
            if (ctx->entries[old_index].type == FM_ITEM_DIR)
            {
                return FM_ERR_NOT_DIRECTORY;
            }
        }
""".replace('\n', '\r\n')
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('f3_02 applied')
