# FILE3-06: every line edit silently converts a CRLF file to LF.
#
# Text_Replace / Text_InsertLine / Text_DeleteLine copy the file through
# Text_ReadLine + Text_WriteLine.  Text_ReadLine drops the CR of a CRLF
# line end (documented, and right for a *reader*), Text_WriteLine always
# appends '\n'.  So editing one word in a CSV with CRLF line ends rewrites
# every line ending in the file: 26 bytes -> 23 bytes in the reproducer,
# and a file whose last line had no newline gets one.  Nothing in the
# result code or the log says so.
#
# The fix keeps Text_ReadLine exactly as documented and gives the three
# rewrite helpers their own faithful reader/writer pair: the ending of
# each line (CRLF / LF / none) is read, reported and written back
# unchanged.  Lone CRs inside a line stay data (FILE-04), the last line
# of a file without a newline stays without one, and an inserted line
# takes the ending of the line it is inserted before (LF in an empty
# file).
import io

p = 'src/fm_text.c'
s = io.open(p, 'r', encoding='utf-8', newline='').read()


def crlf(t):
    return t.replace('\n', '\r\n')


def patch(old, new, what):
    global s
    o = crlf(old)
    n = crlf(new)
    if s.count(o) != 1:
        raise SystemExit('f3_06: %s: needle count %d' % (what, s.count(o)))
    s = s.replace(o, n)


# ------------------------------------------------------------------ #
# 1. the faithful reader/writer, after Text_WriteLine                 #
# ------------------------------------------------------------------ #
old = """    len = strlen(line);
    if (len > 0)
    {
        r = File_Write(f, line, (uint32_t)len, &wr);
        if (r != FM_OK)
        {
            return r;
        }
    }
    r = File_Write(f, "\\n", 1, &wr);
    return r;
}
"""
new = """    len = strlen(line);
    if (len > 0)
    {
        r = File_Write(f, line, (uint32_t)len, &wr);
        if (r != FM_OK)
        {
            return r;
        }
    }
    r = File_Write(f, "\\n", 1, &wr);
    return r;
}

/* Read one line without interpreting its ending.  *crlf says the line
 * ended with CRLF (that CR is not in the buffer), *eol says it ended with
 * a newline at all -- the last line of a file need not.  Used by the
 * three rewrite helpers, which must give back the lines they did not
 * change: Text_ReadLine's documented CR removal turned every CRLF file
 * into an LF file on any edit. */
static FM_Result text_read_raw(FM_File *f, char *buf, size_t size,
                               size_t *len_out, FM_Bool *crlf_out,
                               FM_Bool *eol_out)
{
    size_t n = 0;
    FM_Bool any = FM_FALSE;
    FM_Bool crlf = FM_FALSE;
    FM_Bool eol = FM_FALSE;
    FM_Bool overflow = FM_FALSE;

    for (;;)
    {
        uint32_t rd = 0;
        char c = 0;
        FM_Result r = File_Read(f, &c, 1, &rd);

        if (r == FM_ERR_EOF || (r == FM_OK && rd == 0))
        {
            if (!any)
            {
                return FM_ERR_EOF;
            }
            break;
        }
        if (r != FM_OK)
        {
            return r;
        }
        any = FM_TRUE;
        if (c == '\\n')
        {
            eol = FM_TRUE;
            break;
        }
        if (n + 1 < size)
        {
            buf[n++] = c;
        }
        else
        {
            overflow = FM_TRUE;
        }
    }
    /* the CR of a CRLF belongs to the ending, not to the text -- but only
     * when there really is a '\\n' after it: a CR that ends the file is
     * data (Text_ReadLine has the same rule) */
    if (eol && n > 0 && buf[n - 1] == '\\r')
    {
        n--;
        crlf = FM_TRUE;
    }
    buf[n] = '\\0';
    if (len_out != NULL)
    {
        *len_out = n;
    }
    if (crlf_out != NULL)
    {
        *crlf_out = crlf;
    }
    if (eol_out != NULL)
    {
        *eol_out = eol;
    }
    return overflow ? FM_ERR_TOO_LARGE : FM_OK;
}

/* The matching writer: the text as it came out of text_read_raw(), then
 * the same ending (nothing at all for the last line of a file that had
 * none). */
static FM_Result text_write_raw(FM_File *f, const char *buf, size_t len,
                                FM_Bool crlf, FM_Bool eol)
{
    uint32_t wr = 0;
    FM_Result r;

    if (len > 0)
    {
        r = File_Write(f, buf, (uint32_t)len, &wr);
        if (r != FM_OK)
        {
            return r;
        }
    }
    if (!eol)
    {
        return FM_OK;
    }
    return File_Write(f, crlf ? "\\r\\n" : "\\n", crlf ? 2u : 1u, &wr);
}
"""
patch(old, new, 'raw helpers')

# ------------------------------------------------------------------ #
# 2. Text_Replace                                                     #
# ------------------------------------------------------------------ #
old = """    for (;;)
    {
        size_t line_len = 0;
        size_t pos = 0;

        r = Text_ReadLine(src, line, sizeof(line), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
"""
new = """    for (;;)
    {
        size_t line_len = 0;
        size_t pos = 0;
        FM_Bool line_crlf = FM_FALSE;
        FM_Bool line_eol = FM_FALSE;

        r = text_read_raw(src, line, sizeof(line), &line_len, &line_crlf,
                          &line_eol);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
"""
patch(old, new, 'replace read')

old = """        r = Text_WriteLine(dst, line);
        if (r != FM_OK)
        {
            goto fail;
        }
    }

    r = File_Close(dst);
    dst = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Close(src);
    src = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Rename(tmp, path);
    if (r != FM_OK)
    {
        (void)File_Delete(tmp);
    }
    FM_Core_SetLastError(r);
    return r;

fail:
    if (dst != NULL)
    {
        (void)File_Close(dst);
    }
    if (src != NULL)
    {
        (void)File_Close(src);
    }
    (void)File_Delete(tmp);
    FM_Core_SetLastError(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Line insert / delete                                                */
/* ------------------------------------------------------------------ */
"""
new = """        r = text_write_raw(dst, line, line_len, line_crlf, line_eol);
        if (r != FM_OK)
        {
            goto fail;
        }
    }

    r = File_Close(dst);
    dst = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Close(src);
    src = NULL;
    if (r != FM_OK)
    {
        goto fail;
    }
    r = File_Rename(tmp, path);
    if (r != FM_OK)
    {
        (void)File_Delete(tmp);
    }
    FM_Core_SetLastError(r);
    return r;

fail:
    if (dst != NULL)
    {
        (void)File_Close(dst);
    }
    if (src != NULL)
    {
        (void)File_Close(src);
    }
    (void)File_Delete(tmp);
    FM_Core_SetLastError(r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Line insert / delete                                                */
/* ------------------------------------------------------------------ */
"""
patch(old, new, 'replace write')

# ------------------------------------------------------------------ #
# 3. Text_InsertLine                                                  #
# ------------------------------------------------------------------ #
old = """    for (;;)
    {
        size_t line_len = 0;

        r = Text_ReadLine(src, buf, sizeof(buf), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == at_line)
        {
            r = Text_WriteLine(dst, line);
            if (r != FM_OK)
            {
                goto fail;
            }
        }
        r = Text_WriteLine(dst, buf);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
    if (n < at_line)
    {
        /* append at end */
        r = Text_WriteLine(dst, line);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
"""
new = """    for (;;)
    {
        size_t line_len = 0;
        FM_Bool line_crlf = FM_FALSE;
        FM_Bool line_eol = FM_FALSE;

        r = text_read_raw(src, buf, sizeof(buf), &line_len, &line_crlf,
                          &line_eol);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == at_line)
        {
            /* the new line takes the ending of the line it goes in front
             * of, so a CRLF file stays a CRLF file */
            r = text_write_raw(dst, line, strlen(line), line_crlf, FM_TRUE);
            if (r != FM_OK)
            {
                goto fail;
            }
        }
        last_crlf = line_crlf;
        r = text_write_raw(dst, buf, line_len, line_crlf, line_eol);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
    if (n < at_line)
    {
        /* append at end; an empty file has no style to follow, so LF */
        r = text_write_raw(dst, line, strlen(line), last_crlf, FM_TRUE);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
"""
patch(old, new, 'insert loop')

old = """    char tmp[FM_PATH_MAX];
    char buf[FM_INTERNAL_BUFFER_SIZE];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    size_t n = 0;
    FM_Result r;

    if (path == NULL || line == NULL || at_line == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
"""
new = """    char tmp[FM_PATH_MAX];
    char buf[FM_INTERNAL_BUFFER_SIZE];
    FM_File *src = NULL;
    FM_File *dst = NULL;
    size_t n = 0;
    FM_Bool last_crlf = FM_FALSE;
    FM_Result r;

    if (path == NULL || line == NULL || at_line == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
"""
patch(old, new, 'insert locals')

# ------------------------------------------------------------------ #
# 4. Text_DeleteLine                                                  #
# ------------------------------------------------------------------ #
old = """    for (;;)
    {
        size_t line_len = 0;

        r = Text_ReadLine(src, buf, sizeof(buf), &line_len);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == line)
        {
            continue; /* skip */
        }
        r = Text_WriteLine(dst, buf);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
"""
new = """    for (;;)
    {
        size_t line_len = 0;
        FM_Bool line_crlf = FM_FALSE;
        FM_Bool line_eol = FM_FALSE;

        r = text_read_raw(src, buf, sizeof(buf), &line_len, &line_crlf,
                          &line_eol);
        if (r == FM_ERR_EOF)
        {
            break;
        }
        if (r != FM_OK)
        {
            goto fail;
        }
        n++;
        if (n == line)
        {
            continue; /* skip */
        }
        r = text_write_raw(dst, buf, line_len, line_crlf, line_eol);
        if (r != FM_OK)
        {
            goto fail;
        }
    }
"""
patch(old, new, 'delete loop')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('f3_06: patched %s' % p)
