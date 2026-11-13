/*
 * FileManager - Unified Embedded File & Storage Framework
 * fm_path.c - Path manipulation (no allocation)
 */

#define FM_DEBUG_TAG "path"
#include "fm_debug.h"
#include "fm_path.h"
#include <string.h>

/* Copy src into dst (always NUL-terminated when dst_size > 0).
 * Returns the required length (strlen(src)), so callers can detect
 * truncation with:  fm_str_copy(...) >= dst_size  -> truncated. */
static size_t fm_str_copy(char *dst, size_t dst_size, const char *src)
{
    size_t required = strlen(src);
    size_t n = 0;

    if (dst_size == 0)
    {
        return required;
    }
    while (n + 1 < dst_size && n < required)
    {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return required;
}

FM_Result Path_Join(const char *a, const char *b, char *out, size_t out_size)
{
    size_t la;
    size_t lb;
    size_t b_start = 0;
    size_t need;
    FM_Bool add_sep;

    if (a == NULL || b == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    la = strlen(a);
    lb = strlen(b);
    if (la == 0)
    {
        if (lb + 1 > out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        memmove(out, b, lb + 1);
        return FM_OK;
    }
    if (lb == 0)
    {
        if (la + 1 > out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        memmove(out, a, la + 1);
        return FM_OK;
    }

    /* A component may be passed with a leading separator.  It is still
     * the second component; do not manufacture "//" in the result. */
    while (b_start < lb && b[b_start] == '/')
    {
        b_start++;
    }
    if (b_start == lb)
    {
        if (la + 1 > out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        memmove(out, a, la + 1);
        return FM_OK;
    }

    add_sep = (a[la - 1] != '/') ? FM_TRUE : FM_FALSE;
    need = la + (add_sep ? 1u : 0u) + (lb - b_start);
    if (need + 1 > out_size)
    {
        return FM_ERR_TOO_LARGE;
    }

    /* `out` may be `a` or `b` (joining in place).  The tail goes to its
     * place first: copying `a` first wrote over `b` when out == b, and
     * Path_Join("/dir", buf, buf, n) returned FM_OK with "/dir//dir/..."
     * in buf. */
    memmove(out + la + (add_sep ? 1u : 0u), b + b_start, lb - b_start);
    memmove(out, a, la);
    if (add_sep)
    {
        out[la] = '/';
    }
    out[need] = '\0';
    return FM_OK;
}

/* Where each kept component starts.  A path of fewer than FM_PATH_MAX
 * characters has at most FM_PATH_MAX / 2 components ("a/a/a..."), and
 * every start is below FM_PATH_MAX: the list is that long and its
 * entries that wide, no more.  (It was size_t[FM_PATH_MAX]: 640 bytes
 * of stack on a 32-bit target, under every call that takes a path.) */
#if FM_PATH_MAX <= 256
typedef uint8_t fm_path_off_t;
#elif FM_PATH_MAX <= 65536
typedef uint16_t fm_path_off_t;
#else
typedef size_t fm_path_off_t;
#endif
#define FM_PATH_COMP_MAX ((size_t)FM_PATH_MAX / 2u + 1u)

FM_Result Path_Normalize(char *path, size_t path_size)
{
    char result[FM_PATH_MAX];
    fm_path_off_t offsets[FM_PATH_COMP_MAX];
    size_t input_len = 0;
    size_t ncomp = 0;
    size_t out_len = 0;
    size_t i = 0;
    size_t j;
    FM_Bool absolute;

    if (path == NULL || path_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    while (input_len < path_size && path[input_len] != '\0')
    {
        input_len++;
    }
    if (input_len == path_size || input_len >= FM_PATH_MAX)
    {
        return FM_ERR_TOO_LARGE;
    }

    absolute = (path[0] == '/') ? FM_TRUE : FM_FALSE;
    while (i < input_len)
    {
        size_t start;
        size_t len;

        while (i < input_len && path[i] == '/')
        {
            i++;
        }
        if (i == input_len)
        {
            break;
        }
        start = i;
        while (i < input_len && path[i] != '/')
        {
            i++;
        }
        len = i - start;

        if (len == 1 && path[start] == '.')
        {
            continue;
        }
        if (len == 2 && path[start] == '.' && path[start + 1] == '.')
        {
            size_t prev = (ncomp > 0) ? (size_t)offsets[ncomp - 1] : 0u;

            if (ncomp > 0 &&
                !(path[prev] == '.' &&
                  path[prev + 1] == '.' &&
                  (prev + 2 == input_len ||
                   path[prev + 2] == '/')))
            {
                ncomp--;
            }
            else if (!absolute)
            {
                if (ncomp >= FM_PATH_COMP_MAX)
                {
                    return FM_ERR_TOO_LARGE;
                }
                offsets[ncomp++] = (fm_path_off_t)start;
            }
            continue;
        }
        if (ncomp >= FM_PATH_COMP_MAX)
        {
            return FM_ERR_TOO_LARGE;
        }
        offsets[ncomp++] = (fm_path_off_t)start;
    }

    if (absolute)
    {
        result[out_len++] = '/';
    }
    for (j = 0; j < ncomp; j++)
    {
        size_t start = offsets[j];
        size_t len = 0;

        while (start + len < input_len && path[start + len] != '/')
        {
            len++;
        }
        if (out_len > 0 && result[out_len - 1] != '/')
        {
            if (out_len + 1 >= sizeof(result))
            {
                return FM_ERR_TOO_LARGE;
            }
            result[out_len++] = '/';
        }
        if (out_len + len >= sizeof(result))
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(result + out_len, path + start, len);
        out_len += len;
    }
    if (out_len == 0)
    {
        result[out_len++] = absolute ? '/' : '.';
    }
    if (out_len + 1 > path_size || out_len + 1 > sizeof(result))
    {
        return FM_ERR_TOO_LARGE;
    }
    result[out_len] = '\0';
    memcpy(path, result, out_len + 1);
    return FM_OK;
}

FM_Bool Path_IsAbsolute(const char *path)
{
    return path != NULL && path[0] == '/';
}

FM_Bool Path_IsRelative(const char *path)
{
    return path != NULL && path[0] != '/';
}

FM_Bool Path_IsNormalized(const char *path)
{
    char tmp[FM_PATH_MAX];

    if (path == NULL)
    {
        return FM_FALSE;
    }
    fm_str_copy(tmp, sizeof(tmp), path);
    if (Path_Normalize(tmp, sizeof(tmp)) != FM_OK)
    {
        return FM_FALSE;
    }
    return strcmp(tmp, path) == 0;
}

FM_Result Path_GetDirectory(const char *path, char *out, size_t out_size)
{
    const char *slash;
    size_t len;

    if (path == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    slash = strrchr(path, '/');
    if (slash == NULL)
    {
        /* no directory part */
        out[0] = '\0';
        return FM_OK;
    }
    if (slash == path)
    {
        /* "/name" -> "/" */
        return fm_str_copy(out, out_size, "/") >= out_size ? FM_ERR_TOO_LARGE
                                                           : FM_OK;
    }
    len = (size_t)(slash - path);
    if (len + 1 > out_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return FM_OK;
}

FM_Result Path_GetFilename(const char *path, char *out, size_t out_size)
{
    const char *slash;

    if (path == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    slash = strrchr(path, '/');
    if (slash == NULL)
    {
        return fm_str_copy(out, out_size, path) >= out_size ? FM_ERR_TOO_LARGE
                                                            : FM_OK;
    }
    return fm_str_copy(out, out_size, slash + 1) >= out_size
               ? FM_ERR_TOO_LARGE
               : FM_OK;
}

FM_Result Path_GetExtension(const char *path, char *out, size_t out_size)
{
    const char *slash;
    const char *dot;
    const char *name;
    size_t len;
    size_t i;

    if (path == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }

    slash = strrchr(path, '/');
    name = (slash == NULL) ? path : slash + 1;
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name)
    {
        out[0] = '\0';
        return FM_OK;
    }
    dot++;
    len = strlen(dot);
    if (len + 1 > out_size)
    {
        return FM_ERR_TOO_LARGE;
    }
    for (i = 0; i < len; i++)
    {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[len] = '\0';
    return FM_OK;
}

FM_Result Path_RemoveExtension(char *path, size_t path_size)
{
    const char *slash;
    char *dot;
    char *name;

    if (path == NULL || path_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    {
        size_t len = 0;
        while (len < path_size && path[len] != '\0') len++;
        if (len == path_size)
        {
            return FM_ERR_TOO_LARGE;
        }
    }
    slash = strrchr(path, '/');
    name = (slash == NULL) ? path : (char *)(slash + 1);
    dot = strrchr(name, '.');
    if (dot != NULL && dot != name)
    {
        *dot = '\0';
    }
    return FM_OK;
}

FM_Result Path_GetParent(const char *path, char *out, size_t out_size)
{
    const char *slash;

    if (path == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (path[0] == '\0')
    {
        out[0] = '\0';
        return FM_OK;
    }

    slash = strrchr(path, '/');
    if (slash == NULL)
    {
        out[0] = '\0';
        return FM_OK;
    }
    if (slash == path)
    {
        return fm_str_copy(out, out_size, "/") >= out_size ? FM_ERR_TOO_LARGE
                                                           : FM_OK;
    }
    {
        size_t len = (size_t)(slash - path);
        if (len + 1 > out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(out, path, len);
        out[len] = '\0';
        return FM_OK;
    }
}

FM_Result Path_GetRoot(const char *path, char *out, size_t out_size)
{
    const char *first;

    if (path == NULL || out == NULL || out_size == 0)
    {
        return FM_ERR_INVALID_ARGUMENT;
    }
    if (path[0] == '/')
    {
        return fm_str_copy(out, out_size, "/") >= out_size ? FM_ERR_TOO_LARGE
                                                           : FM_OK;
    }
    first = strchr(path, '/');
    if (first == NULL)
    {
        return fm_str_copy(out, out_size, path) >= out_size ? FM_ERR_TOO_LARGE
                                                            : FM_OK;
    }
    {
        size_t len = (size_t)(first - path);
        if (len + 1 > out_size)
        {
            return FM_ERR_TOO_LARGE;
        }
        memcpy(out, path, len);
        out[len] = '\0';
        return FM_OK;
    }
}
