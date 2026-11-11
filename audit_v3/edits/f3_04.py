"""FILE3-04: a line longer than the fixed line buffer.

Text_ReadLine() consumes the whole line even when it does not fit (it
returns FM_ERR_TOO_LARGE with the first line_size-1 characters).  The two
callers that only *look* at lines -- Text_CountLines() and Text_Find() --
treated that as a fatal error, so one long line made the count 0 for the
whole file, and made the search stop.

Runs with the copied library tree as its working directory.
"""
import io

P = 'src/fm_text.c'


def crlf(t):
    return t.replace('\n', '\r\n')


s = io.open(P, 'r', encoding='utf-8', newline='').read()


def swap(old, new, tag):
    global s
    o = crlf(old)
    n = crlf(new)
    if s.count(o) != 1:
        raise SystemExit('%s: %d matches' % (tag, s.count(o)))
    s = s.replace(o, n)


# ---- Text_CountLines ------------------------------------------------
swap("""    for (;;)
    {
        char line[FM_NAME_MAX * 2];

        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
    }
""",
"""    for (;;)
    {
        /* Lines are counted, not kept, so the buffer only has to hold a
         * line far enough to notice where it ends -- and Text_ReadLine()
         * consumes a line that does not fit anyway (it returns
         * FM_ERR_TOO_LARGE with the part that fits).  It used to be
         * FM_NAME_MAX * 2 = 128 bytes and the whole call failed as soon
         * as ONE line was longer than 127 characters: a log file, a CSV
         * row or a JSON line made the count 0. */
        char line[FM_INTERNAL_BUFFER_SIZE];

        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK && r != FM_ERR_TOO_LARGE)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
    }
""", 'count lines')

# ---- Text_Find ------------------------------------------------------
swap("""        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
        if (strstr(line, needle) != NULL)
""",
"""        r = Text_ReadLine(f, line, sizeof(line), NULL);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK && r != FM_ERR_TOO_LARGE)
        {
            (void)File_Close(f);
            return r;
        }
        n++;
        /* FM_ERR_TOO_LARGE means the line was longer than the buffer:
         * the last (FM_INTERNAL_BUFFER_SIZE - 1) bytes of it were not
         * examined.  The line is still searched and the count still moves
         * on, instead of the whole search stopping at that line. */
        if (strstr(line, needle) != NULL)
""", 'find')

io.open(P, 'w', encoding='utf-8', newline='').write(s)
print('f3_04 applied')
